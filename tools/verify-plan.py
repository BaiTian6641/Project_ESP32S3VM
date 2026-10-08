#!/usr/bin/env python3
"""Check roadmap traceability and dependencies; not hardware qualification."""

import argparse
import copy
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
DIMENSIONS = {"positive", "negative", "reset", "compatibility", "reference"}
SCOPES = {
    "ux", "peripherals", "digital_nets", "analog_voltages", "adc", "wifi",
    "ble", "simd", "recent_idf_compatibility", "hardware_reference",
}
CROSS_CUTTING = {"HW", "VERSION", "TRANSPORT", "TRACE", "REFERENCE"}


def check(manifest):
    errors = []

    def require(condition, message):
        if not condition:
            errors.append(message)

    def document_exists(name):
        if not isinstance(name, str) or not name:
            return False
        path = (ROOT / name).resolve()
        return path.is_relative_to(ROOT) and path.is_file()

    require(manifest.get("schema_version") == 1, "Unsupported coverage schema")
    require(set(manifest.get("goal_scope", [])) == SCOPES, "User scope is incomplete")
    for name in manifest.get("documents", []):
        require(document_exists(name), f"Missing/outside-workspace document: {name}")
    require(document_exists(manifest.get("plan")), "Missing master plan")
    require(document_exists(manifest.get("review")), "Missing independent review log")

    plan_path = ROOT / manifest.get("plan", "missing")
    expected = set()
    plan_text = ""
    if document_exists(manifest.get("plan")):
        plan_text = plan_path.read_text(encoding="utf-8")
        expected = set(re.findall(r"^\| ([A-Z][A-Z0-9]*) / ", plan_text, re.M)) - {"ID"}
    require(bool(expected), "Master plan has no functionality coverage rows")

    milestones = manifest.get("milestones", [])
    milestone_ids = [item.get("id") for item in milestones]
    require(len(set(milestone_ids)) == len(milestone_ids), "Duplicate milestone IDs")
    require("M0" in milestone_ids and "M11" in milestone_ids, "Missing baseline/release gate")
    graph = {item.get("id"): item.get("dependencies", []) for item in milestones}
    table_graph = {
        key: re.findall(r"M\d+[A-Z]?", parents)
        for key, parents in re.findall(r"^\| (M\d+[A-Z]?):[^|]+ \| ([^|]+) \|", plan_text, re.M)
    }
    require(set(table_graph) == set(graph), "Master table and manifest milestone IDs disagree")
    for key, parents in table_graph.items():
        require(set(parents) == set(graph.get(key, [])), f"{key}: master/manifest dependencies disagree")
    for node, parents in graph.items():
        for parent in parents:
            require(parent in graph, f"{node}: unknown dependency {parent}")
        require(node not in parents, f"{node}: self dependency")

    visiting, visited = set(), set()

    def visit(node):
        if node in visiting:
            errors.append(f"Milestone dependency cycle at {node}")
            return
        if node in visited or node not in graph:
            return
        visiting.add(node)
        for parent in graph[node]:
            visit(parent)
        visiting.remove(node)
        visited.add(node)

    for node in graph:
        visit(node)

    def ancestors(node, seen=None):
        seen = set() if seen is None else seen
        for parent in graph.get(node, []):
            if parent not in seen:
                seen.add(parent)
                ancestors(parent, seen)
        return seen

    release_stages = ancestors("M11") | {"M11"}
    package_ids_by_doc = {}
    for name in manifest.get("documents", [])[1:]:
        if not document_exists(name):
            continue
        content = (ROOT / name).read_text(encoding="utf-8")
        ids = set(re.findall(r"^#{2,3} ([A-Z][A-Z0-9]*-\d{2})(?:\s|:)", content, re.M))
        ids |= set(re.findall(r"^\| ([A-Z][A-Z0-9]*-\d{2}) \|", content, re.M))
        for key in ids:
            require(key not in package_ids_by_doc, f"Ambiguous work-package ID {key}")
            package_ids_by_doc[key] = name
    packages = manifest.get("work_packages", [])
    package_ids = [item.get("id") for item in packages]
    require(len(set(package_ids)) == len(package_ids), "Duplicate work-package IDs")
    require(set(package_ids) == set(package_ids_by_doc), "Detailed work-package coverage is incomplete")
    package_by_id = {item.get("id"): item for item in packages}
    promoted = {"native-tested", "hardware-compared", "qualified"}
    valid_statuses = {"planned", "in-progress", "catalogued", "implemented"} | promoted
    for package in packages:
        key = package.get("id", "?")
        require(package.get("document") == package_ids_by_doc.get(key), f"{key}: wrong workstream source")
        require(bool(package.get("milestones")), f"{key}: no milestone assignment")
        for stage in package.get("milestones", []):
            require(stage in graph and stage in release_stages, f"{key}: unknown/omitted release milestone {stage}")
        require(package.get("status") in valid_statuses, f"{key}: invalid maturity/status")
        if package.get("status") in promoted:
            require(bool(package.get("evidence")), f"{key}: promoted without evidence")

    rows = manifest.get("requirements", [])
    row_ids = [item.get("id") for item in rows]
    require(len(set(row_ids)) == len(row_ids), "Duplicate requirement IDs")
    require(set(row_ids) == expected,
            f"Coverage mismatch: missing={sorted(expected-set(row_ids))}; extra={sorted(set(row_ids)-expected)}")
    for row in rows:
        key = row.get("id", "?")
        require(document_exists(row.get("document")), f"{key}: missing detailed workstream")
        acceptance = row.get("acceptance", {})
        require(set(acceptance) == DIMENSIONS, f"{key}: incomplete acceptance dimensions")
        for dimension in DIMENSIONS:
            require(isinstance(acceptance.get(dimension), str) and len(acceptance[dimension].strip()) >= 20,
                    f"{key}: empty/insufficient {dimension} acceptance")
        stages = row.get("milestones", [])
        require(bool(stages), f"{key}: no assigned milestone")
        for stage in stages:
            require(stage in graph, f"{key}: unknown milestone {stage}")
            require(stage in release_stages, f"{key}: milestone {stage} omitted from release dependencies")
        status = row.get("status")
        require(status in valid_statuses,
                f"{key}: invalid evidence status")
        required_packages = row.get("required_packages", [])
        require(bool(required_packages), f"{key}: no granular qualification requirements")
        for package_id in required_packages:
            require(package_id in package_by_id, f"{key}: unknown work package {package_id}")
        if status in promoted:
            require(bool(row.get("evidence")), f"{key}: promoted without evidence")
            ranks = {"planned": 0, "in-progress": 0, "catalogued": 0, "implemented": 1,
                     "native-tested": 2, "hardware-compared": 3, "qualified": 4}
            for package_id in required_packages:
                package = package_by_id.get(package_id, {})
                require(ranks.get(package.get("status"), -1) >= ranks[status],
                        f"{key}: promoted beyond package {package_id}")

    cross = manifest.get("cross_cutting", [])
    require({item.get("id") for item in cross} == CROSS_CUTTING, "Missing shared/hardware/version gate")
    for item in cross:
        require(document_exists(item.get("document")), f"{item.get('id')}: missing cross-cutting document")
        require(len(item.get("acceptance", "")) >= 20, f"{item.get('id')}: missing acceptance")
        require(bool(item.get("required_packages")), f"{item.get('id')}: missing package references")
        for package_id in item.get("required_packages", []):
            require(package_id in package_by_id, f"{item.get('id')}: unknown shared package {package_id}")
    covered_packages = {
        key for item in rows + cross for key in item.get("required_packages", [])
    }
    require(covered_packages == set(package_ids), "Work packages lack requirement/shared-gate traceability")
    return errors


def self_test(manifest):
    mutations = []
    missing = copy.deepcopy(manifest)
    missing["requirements"].pop()
    mutations.append(("missing functionality", missing))
    cycle = copy.deepcopy(manifest)
    cycle["milestones"][0]["dependencies"] = ["M11"]
    mutations.append(("dependency cycle", cycle))
    acceptance = copy.deepcopy(manifest)
    acceptance["requirements"][0]["acceptance"].pop("negative")
    mutations.append(("missing failure acceptance", acceptance))
    evidence = copy.deepcopy(manifest)
    evidence["requirements"][0]["status"] = "qualified"
    mutations.append(("unsupported promotion", evidence))
    outside = copy.deepcopy(manifest)
    outside["requirements"][0]["document"] = "../outside.md"
    mutations.append(("missing/external document", outside))
    missing_package = copy.deepcopy(manifest)
    missing_package["work_packages"].pop()
    mutations.append(("missing detailed work package", missing_package))
    disagreement = copy.deepcopy(manifest)
    disagreement["milestones"][1]["dependencies"] = []
    mutations.append(("master/manifest disagreement", disagreement))
    premature = copy.deepcopy(manifest)
    premature["requirements"][0]["status"] = "qualified"
    premature["requirements"][0]["evidence"] = ["init-only"]
    mutations.append(("aggregate promotion before all packages", premature))
    for name, mutated in mutations:
        if not check(mutated):
            raise AssertionError(f"Validator accepted {name}")
    return len(mutations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    manifest = json.loads((ROOT / "docs/plan-coverage.json").read_text(encoding="utf-8"))
    errors = check(manifest)
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"PASS: {len(manifest['requirements'])} functionality rows, "
          f"{len(manifest['work_packages'])} stable work packages, "
          f"{len(manifest['milestones'])} acyclic milestones, "
          f"{len(manifest['cross_cutting'])} shared gates; acceptance dimensions present.")
    if args.self_test:
        print(f"PASS: {self_test(manifest)} omission/cycle/evidence regression cases rejected.")
    print("Structural validation only; detailed phase/interface dependencies need independent review. "
          "Technical correctness requires source review; "
          "implementation support requires actual firmware/reference evidence.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
