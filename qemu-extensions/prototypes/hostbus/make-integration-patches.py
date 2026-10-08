#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reproducible patch suggestions; reads pinned sources, never edits them."""
import difflib
from pathlib import Path
import subprocess
import sys

BASE_COMMIT = "40edccac415693c5130f91c01d84176ae6008566"
directory = Path(__file__).resolve().parent
base = Path(sys.argv[1]).resolve()
revision = subprocess.check_output(["git", "-C", str(base), "rev-parse", "HEAD"], text=True).strip()
if revision != BASE_COMMIT:
    raise SystemExit("Patch generation requires the exact locked base commit")


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise AssertionError(f"Pinned source context changed: {before!r}")
    return text.replace(before, after, 1)


def emit(name, changes):
    patches = []
    for file_name, transform in changes:
        original = (base / file_name).read_text()
        changed = transform(original)
        patches.append(f"diff --git a/{file_name} b/{file_name}\n")
        patches.extend(difflib.unified_diff(original.splitlines(True), changed.splitlines(True),
                                           fromfile="a/" + file_name, tofile="b/" + file_name))
    (directory / name).write_text("".join(patches))
    print(name)


def debug_preflight(text):
    text = replace_once(text, '#include "sysemu/runstate.h"\n',
                        '#include "sysemu/runstate.h"\n'
                        '#if defined(CONFIG_TCG) && !defined(CONFIG_USER_ONLY)\n'
                        '#include "sysemu/hostbus-probe.h"\n#endif\n')
    helper = '''/* Reject execution commands before PC, signal, step or replay mutation. */
static bool hostbus_execution_preflight(void)
{
#if defined(CONFIG_TCG) && !defined(CONFIG_USER_ONLY)
    if (esp32s3vm_hostbus_resume_blocked()) {
        gdb_put_packet("E16");
        return false;
    }
#endif
    return true;
}

'''
    text = replace_once(text, 'GDBState gdbserver_state;\n\n',
                        'GDBState gdbserver_state;\n\n' + helper)
    for name in ("handle_continue", "handle_step", "handle_backward"):
        entry = f"static void {name}(GArray *params, void *user_ctx)\n{{\n"
        text = replace_once(text, entry,
                            entry + '    if (!hostbus_execution_preflight()) {\n        return;\n    }\n')
    text = replace_once(text, '    unsigned long signal = 0;\n\n',
                        '    unsigned long signal = 0;\n\n'
                        '    if (!hostbus_execution_preflight()) {\n        return;\n    }\n\n')
    entry = 'static void handle_v_cont(GArray *params, void *user_ctx)\n{\n    int res;\n\n'
    text = replace_once(text, entry,
                        entry + '    if (!hostbus_execution_preflight()) {\n        return;\n    }\n\n')
    return text


def include_guard(text):
    return replace_once(text, '#include "sysemu/runstate.h"\n',
                        '#include "sysemu/runstate.h"\n#ifdef CONFIG_TCG\n'
                        '#include "sysemu/hostbus-probe.h"\n#endif\n')


def snapshot_guard(text):
    text = include_guard(text)
    entry = '    MigrationIncomingState *mis = migration_incoming_get_current();\n\n'
    # The same declaration occurs in other functions; anchor within load_snapshot.
    start = text.index('bool load_snapshot(')
    section = replace_once(text[start:], entry, entry + '''#ifdef CONFIG_TCG
    if (esp32s3vm_hostbus_present()) {
        error_setg(errp, "Hostbus probe cannot restore snapshots with external-peer state");
        return false;
    }
#endif

''')
    text = text[:start] + section
    start = text.index('void qmp_snapshot_load(')
    end = text.index('\n}', start) + 2
    section = replace_once(text[start:end], '    SnapshotJob *s;\n\n', '    SnapshotJob *s;\n\n' + '''#ifdef CONFIG_TCG
    if (esp32s3vm_hostbus_present()) {
        error_setg(errp, "Hostbus probe cannot restore snapshots with external-peer state");
        return;
    }
#endif

''')
    return text[:start] + section + text[end:]


def incoming_guard(text):
    text = include_guard(text)
    start = text.index('static void qemu_start_incoming_migration(')
    end = text.index('\n}', start) + 2
    section = text[start:end]
    entry = '    MigrationIncomingState *mis = migration_incoming_get_current();\n\n'
    section = replace_once(section, entry, entry + '''#ifdef CONFIG_TCG
    if (esp32s3vm_hostbus_present()) {
        error_setg(errp, "Hostbus probe cannot accept incoming restore with external-peer state");
        return;
    }
#endif

''')
    text = text[:start] + section + text[end:]
    start = text.index('void qmp_migrate_incoming(')
    end = text.index('\n}', start) + 2
    section = text[start:end]
    section = replace_once(section, entry, entry + '''#ifdef CONFIG_TCG
    if (esp32s3vm_hostbus_present()) {
        error_setg(errp, "Hostbus probe cannot accept incoming restore with external-peer state");
        return;
    }
#endif

''')
    return text[:start] + section + text[end:]


def hmp_snapshot_guard(text):
    text = include_guard(text)
    start = text.index('void hmp_loadvm(')
    end = text.index('\n}', start) + 2
    section = text[start:end]
    section = replace_once(section, '    Error *err = NULL;\n\n',
                           '    Error *err = NULL;\n\n' + '''#ifdef CONFIG_TCG
    if (esp32s3vm_hostbus_present()) {
        error_setg(&err, "Hostbus probe cannot restore snapshots with external-peer state");
        hmp_handle_error(mon, err);
        return;
    }
#endif

''')
    return text[:start] + section + text[end:]


def qom_options(text):
    properties = '''##
# @Esp32s3HostbusProbeProperties:
#
# Properties for the controlled external-peer hostbus prototype object.
# This object exposes QOM debug control only, with no guest MMIO mapping.
#
# @chardev: identifier of the dedicated socket character backend
#
# @watchdog-ms: canonical decimal realtime watchdog milliseconds,
#     1 through 600000 (default: "5000")
#
# Since: 9.2
##
{ 'struct': 'Esp32s3HostbusProbeProperties',
  'if': 'CONFIG_TCG',
  'data': { 'chardev': 'str', '*watchdog-ms': 'str' } }

'''
    text = replace_once(text, '##\n# @ObjectType:\n', properties + '##\n# @ObjectType:\n')
    text = replace_once(text, "    'dbus-vmstate',\n", "    'dbus-vmstate',\n"
                        "    { 'name': 'esp32s3-hostbus-probe', 'if': 'CONFIG_TCG' },\n")
    text = replace_once(text, "      'dbus-vmstate':               'DBusVMStateProperties',\n",
                        "      'dbus-vmstate':               'DBusVMStateProperties',\n"
                        "      'esp32s3-hostbus-probe':       { 'type': 'Esp32s3HostbusProbeProperties',\n"
                        "                                      'if': 'CONFIG_TCG' },\n")
    return text


emit("gdb-execution-preflight.patch", [("gdbstub/gdbstub.c", debug_preflight)])
emit("state-restore-guard.patch", [("migration/savevm.c", snapshot_guard),
                                   ("migration/migration.c", incoming_guard),
                                   ("migration/migration-hmp-cmds.c", hmp_snapshot_guard)])
emit("qom-options.patch", [("qapi/qom.json", qom_options)])
