/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "net-adapter.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* H-NET-01 adapter suite: analytic v3-graph cases plus strict rejection and
 * determinism evidence. No claim about MCU registers, firmware or ADC. */

static unsigned assertions;
#define CHECK(condition) do { ++assertions; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define NEAR(actual, expected, tolerance) \
    CHECK(isfinite(actual) && fabs((actual) - (expected)) <= (tolerance))

static EnAdptId eid(const char *text)
{
    EnAdptId id;
    size_t length = strlen(text);
    CHECK(length <= EN_ADPT_ID_MAX);
    memset(&id, 0, sizeof(id));
    memcpy(id.text, text, length + 1);
    return id;
}

static EnAdptTerminal mk_term(const char *id, const char *role,
                              EnAdptTerminalDomain domain,
                              EnAdptTerminalDirection direction)
{
    EnAdptTerminal terminal;
    memset(&terminal, 0, sizeof(terminal));
    terminal.id = eid(id);
    terminal.role = role;
    terminal.domain = domain;
    terminal.direction = direction;
    return terminal;
}

static EnAdptParameter mk_param(const char *name, double value, EnAdptUnit unit)
{
    EnAdptParameter parameter;
    parameter.name = name;
    parameter.quantity.value = value;
    parameter.quantity.unit = unit;
    return parameter;
}

static EnAdptComponent mk_comp(const char *id, EnAdptKind kind,
                               const EnAdptTerminal *terminals,
                               size_t terminal_count,
                               const EnAdptParameter *parameters,
                               size_t parameter_count)
{
    EnAdptComponent component;
    memset(&component, 0, sizeof(component));
    component.id = eid(id);
    component.kind = kind;
    component.terminals = terminals;
    component.terminal_count = terminal_count;
    component.parameters = parameters;
    component.parameter_count = parameter_count;
    return component;
}

static EnAdptNet mk_net(const char *id, const EnAdptId *endpoints,
                        size_t endpoint_count)
{
    EnAdptNet net;
    memset(&net, 0, sizeof(net));
    net.id = eid(id);
    net.endpoints = endpoints;
    net.endpoint_count = endpoint_count;
    return net;
}

static EnAdptGraph mk_graph(const EnAdptComponent *components,
                            size_t component_count, const EnAdptNet *nets,
                            size_t net_count)
{
    EnAdptGraph graph;
    memset(&graph, 0, sizeof(graph));
    graph.abi_version = EN_ADPT_ABI_VERSION;
    graph.components = components;
    graph.component_count = component_count;
    graph.nets = nets;
    graph.net_count = net_count;
    return graph;
}

static void expect_fail_at(const EnAdptGraph *graph, EnAdptStatus want,
                           const char *offender, EnAdptResult *result,
                           int caller_line)
{
    EnAdptStatus got = en_adpt_solve(graph, result);
    if (got != want) {
        fprintf(stderr, "FAIL want=%d got=%d diag=%s (caller line %d)\n",
                (int)want, (int)got, result->diagnostic, caller_line);
        exit(1);
    }
    ++assertions;
    CHECK(result->status == want);
    if (offender && strcmp(result->offender, offender) != 0) {
        fprintf(stderr, "FAIL offender want=%s got=%s diag=%s (caller line %d)\n",
                offender, result->offender, result->diagnostic, caller_line);
        exit(1);
    }
    CHECK(result->diagnostic[0] != '\0');
    for (size_t i = 0; i < result->net_count && i < 4; ++i) {
        CHECK(isnan(result->nets[i].voltage_v));
    }
    for (size_t i = 0; i < result->terminal_count && i < 4; ++i) {
        CHECK(isnan(result->terminals[i].voltage_v));
    }
}

#define expect_fail(graph, want, offender, result) \
    expect_fail_at((graph), (want), (offender), (result), __LINE__)

#define CHECK_SOLVE(graph, want, result) do { \
    EnAdptStatus solve_status_ = en_adpt_solve(graph, result); \
    if (solve_status_ != (want)) { \
        fprintf(stderr, "FAIL solve want=%d got=%d diag=%s (line %d)\n", \
                (int)(want), (int)solve_status_, (result)->diagnostic, __LINE__); \
        exit(1); \
    } \
    ++assertions; \
} while (0)

static void abi_and_empty_graph(void)
{
    EnAdptResult result;
    EnAdptGraph graph;

    CHECK(en_adpt_solve(NULL, &result) == EN_ADPT_INVALID_ABI);
    graph = mk_graph(NULL, 0, NULL, 0);
    CHECK(en_adpt_solve(&graph, NULL) == EN_ADPT_INVALID_ABI);
    graph.abi_version = EN_ADPT_ABI_VERSION + 1;
    CHECK_SOLVE(&graph, EN_ADPT_INVALID_ABI, &result);
    CHECK(result.diagnostic[0] != '\0');

    /* Non-NULL arrays are required when counts are nonzero. */
    graph = mk_graph(NULL, 1, NULL, 0);
    CHECK_SOLVE(&graph, EN_ADPT_INVALID_ABI, &result);
    graph = mk_graph(NULL, 0, NULL, 1);
    CHECK_SOLVE(&graph, EN_ADPT_INVALID_ABI, &result);

    /* An entirely empty connectivity projection is valid: reference only. */
    graph = mk_graph(NULL, 0, NULL, 0);
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    CHECK(result.node_count == 1 && result.element_count == 0);
    CHECK(result.primitive_count == 0 && result.net_count == 0);

}

static void id_grammar_and_duplicates(void)
{
    EnAdptResult result;
    EnAdptGraph graph;
    EnAdptTerminal gt[1], rt[2];
    EnAdptComponent components[2];
    EnAdptId endpoints[2];
    EnAdptNet nets[1];
    EnAdptParameter rp[1];

    gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                    EN_ADPT_DIRECTION_UNSPECIFIED);
    rt[0] = mk_term("R1.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    rt[1] = mk_term("R1.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    rp[0] = mk_param("resistance", 1000, EN_ADPT_UNIT_OHM);
    components[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
    components[1] = mk_comp("R1", EN_ADPT_KIND_RESISTOR, rt, 2, rp, 1);
    endpoints[0] = eid("G1.ref");
    endpoints[1] = eid("R1.a");
    nets[0] = mk_net("LOOP", endpoints, 2);
    graph = mk_graph(components, 2, nets, 1);

    /* Bad component ids: empty, leading punctuation, invalid character. */
    components[1].id = eid("");
    expect_fail(&graph, EN_ADPT_INVALID_ID, "", &result);
    components[1].id = eid(".hidden");
    expect_fail(&graph, EN_ADPT_INVALID_ID, ".hidden", &result);
    components[1].id = eid("R 1");
    expect_fail(&graph, EN_ADPT_INVALID_ID, "R 1", &result);

    /* Over-long id and an unterminated fixed array. */
    memset(components[1].id.text, 'a', sizeof(components[1].id.text));
    expect_fail(&graph, EN_ADPT_INVALID_ID, NULL, &result);
    CHECK(strlen(result.offender) == EN_ADPT_ID_MAX);
    components[1].id = eid("R1");

    /* Duplicate component, net and terminal ids. */
    {
        EnAdptTerminal gt2[1];
        EnAdptComponent dup[3];
        gt2[0] = mk_term("G2.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                         EN_ADPT_DIRECTION_UNSPECIFIED);
        dup[0] = components[0];
        dup[1] = components[1];
        dup[2] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt2, 1, NULL, 0);
        graph = mk_graph(dup, 3, nets, 1);
        expect_fail(&graph, EN_ADPT_DUPLICATE_ID, "G1", &result);
    }
    {
        EnAdptId loop2[2];
        EnAdptNet two_nets[2];
        loop2[0] = eid("G1.ref");
        loop2[1] = eid("R1.a");
        two_nets[0] = nets[0];
        two_nets[1] = mk_net("LOOP", loop2, 2);
        graph = mk_graph(components, 2, two_nets, 2);
        expect_fail(&graph, EN_ADPT_DUPLICATE_ID, "LOOP", &result);
    }
    {
        EnAdptTerminal clash[2];
        EnAdptComponent twins[3];
        clash[0] = mk_term("R1.a", "a", EN_ADPT_DOMAIN_PASSIVE,
                           EN_ADPT_DIRECTION_PASSIVE);
        clash[1] = mk_term("R9.b", "b", EN_ADPT_DOMAIN_PASSIVE,
                           EN_ADPT_DIRECTION_PASSIVE);
        twins[0] = components[0];
        twins[1] = components[1];
        twins[2] = mk_comp("R9", EN_ADPT_KIND_RESISTOR, clash, 2, rp, 1);
        graph = mk_graph(twins, 3, nets, 1);
        expect_fail(&graph, EN_ADPT_DUPLICATE_ID, "R1.a", &result);
    }

    /* Bad net id and bad endpoint grammar (clean two-component graph). */
    components[1].id = eid("R1");
    nets[0] = mk_net("", endpoints, 2);
    graph = mk_graph(components, 2, nets, 1);
    expect_fail(&graph, EN_ADPT_INVALID_ID, "", &result);
    endpoints[1] = eid("R1.a/b c");
    nets[0] = mk_net("LOOP", endpoints, 2);
    expect_fail(&graph, EN_ADPT_INVALID_ID, "R1.a/b c", &result);
    endpoints[1] = eid("R1.a");
    nets[0] = mk_net("LOOP", endpoints, 2);

    /* Wrong role structure: a resistor without an 'a'/'b' pair. */
    rt[0].role = "p";
    expect_fail(&graph, EN_ADPT_TERMINAL_CONFLICT, "R1", &result);
    rt[0].role = "a";
}

static void endpoint_resolution_failures(void)
{
    EnAdptResult result;
    EnAdptGraph graph;
    EnAdptTerminal gt[1], rt[2];
    EnAdptComponent components[2];
    EnAdptId gnd_eps[2], mid_eps[2];
    EnAdptNet nets[2];
    EnAdptParameter rp[1];

    gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                    EN_ADPT_DIRECTION_UNSPECIFIED);
    rt[0] = mk_term("R1.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    rt[1] = mk_term("R1.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    rp[0] = mk_param("resistance", 1000, EN_ADPT_UNIT_OHM);
    components[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
    components[1] = mk_comp("R1", EN_ADPT_KIND_RESISTOR, rt, 2, rp, 1);
    gnd_eps[0] = eid("G1.ref");
    gnd_eps[1] = eid("R1.a");
    nets[0] = mk_net("GND", gnd_eps, 2);
    mid_eps[0] = eid("R1.b");
    mid_eps[1] = eid("GHOST.1");
    nets[1] = mk_net("MID", mid_eps, 2);
    graph = mk_graph(components, 2, nets, 2);

    /* Unknown endpoint names an id no component terminal carries. */
    expect_fail(&graph, EN_ADPT_UNKNOWN_ENDPOINT, "GHOST.1", &result);

    /* Duplicate endpoint within one net. */
    mid_eps[1] = eid("R1.b");
    nets[1] = mk_net("MID", mid_eps, 2);
    expect_fail(&graph, EN_ADPT_DUPLICATE_ENDPOINT, "R1.b", &result);

    /* One terminal shared by two nets. */
    mid_eps[1] = eid("R1.a");
    nets[1] = mk_net("MID", mid_eps, 2);
    expect_fail(&graph, EN_ADPT_TERMINAL_CONFLICT, "R1.a", &result);

    /* Relative solution with an element but no reference anywhere: the
     * source loop drives a real current while both voltages stay NaN. */
    {
        EnAdptTerminal vt[2];
        EnAdptParameter vp[1];
        EnAdptComponent pair[2];
        EnAdptId a_eps[2], m_eps[2];
        EnAdptNet pair_nets[2];
        vt[0] = mk_term("VS.p", "p", EN_ADPT_DOMAIN_POWER,
                        EN_ADPT_DIRECTION_UNSPECIFIED);
        vt[1] = mk_term("VS.n", "n", EN_ADPT_DOMAIN_GROUND,
                        EN_ADPT_DIRECTION_UNSPECIFIED);
        vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
        pair[0] = mk_comp("VS", EN_ADPT_KIND_VOLTAGE_SOURCE, vt, 2, vp, 1);
        pair[1] = components[1];
        a_eps[0] = eid("VS.p");
        a_eps[1] = eid("R1.a");
        m_eps[0] = eid("VS.n");
        m_eps[1] = eid("R1.b");
        pair_nets[0] = mk_net("A", a_eps, 2);
        pair_nets[1] = mk_net("M", m_eps, 2);
        graph = mk_graph(pair, 2, pair_nets, 2);
        CHECK_SOLVE(&graph, EN_ADPT_FLOATING, &result);
        CHECK(isnan(result.nets[0].voltage_v));
        CHECK(isnan(result.nets[1].voltage_v));
        CHECK(result.nets[1].floating == 1);
        NEAR(result.primitives[1].current_a, 0.0033, 1e-15);
    }
}

static void divider_units_and_mapping(void)
{
    EnAdptResult result;
    EnAdptGraph graph;
    EnAdptTerminal gt[1], vt[2], r1t[2], r2t[2];
    EnAdptParameter vp[1], r1p[1], r2p[1];
    EnAdptComponent components[4];
    EnAdptId gnd_eps[3], vplus_eps[2], mid_eps[2];
    EnAdptNet nets[3];

    gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                    EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[0] = mk_term("VS.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[1] = mk_term("VS.n", "n", EN_ADPT_DOMAIN_GROUND, EN_ADPT_DIRECTION_UNSPECIFIED);
    r1t[0] = mk_term("R1.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    r1t[1] = mk_term("R1.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    r2t[0] = mk_term("R2.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    r2t[1] = mk_term("R2.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
    r1p[0] = mk_param("resistance", 10000, EN_ADPT_UNIT_OHM);
    r2p[0] = mk_param("resistance", 10000, EN_ADPT_UNIT_OHM);
    components[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
    components[1] = mk_comp("VS", EN_ADPT_KIND_VOLTAGE_SOURCE, vt, 2, vp, 1);
    components[2] = mk_comp("R1", EN_ADPT_KIND_RESISTOR, r1t, 2, r1p, 1);
    components[3] = mk_comp("R2", EN_ADPT_KIND_RESISTOR, r2t, 2, r2p, 1);
    gnd_eps[0] = eid("G1.ref");
    gnd_eps[1] = eid("VS.n");
    gnd_eps[2] = eid("R2.b");
    vplus_eps[0] = eid("VS.p");
    vplus_eps[1] = eid("R1.a");
    mid_eps[0] = eid("R1.b");
    mid_eps[1] = eid("R2.a");
    nets[0] = mk_net("GND", gnd_eps, 3);
    nets[1] = mk_net("VPLUS", vplus_eps, 2);
    nets[2] = mk_net("MID", mid_eps, 2);
    graph = mk_graph(components, 4, nets, 3);

    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    CHECK(result.nets[0].is_reference == 1 && result.nets[0].node == 0);
    CHECK(result.nets[0].voltage_v == 0.0);
    NEAR(result.nets[1].voltage_v, 3.3, 1e-12);
    NEAR(result.nets[2].voltage_v, 1.65, 1e-12);
    CHECK(result.nets[2].floating == 0 && result.nets[2].node == 2);
    /* Flattened terminals: G1.ref, VS.p, VS.n, R1.a, R1.b, R2.a, R2.b. */
    CHECK(result.terminal_count == 7);
    NEAR(result.terminals[3].voltage_v, 3.3, 1e-12);
    NEAR(result.terminals[4].voltage_v, 1.65, 1e-12);
    NEAR(result.terminals[5].voltage_v, 1.65, 1e-12);
    CHECK(result.terminals[4].on_net == 1);
    /* Primitives in component order: VS source, R1, R2. */
    CHECK(result.primitive_count == 3);
    CHECK(result.node_count == 3 && result.element_count == 3);
    NEAR(result.primitives[0].current_a, -0.000165, 1e-15);
    NEAR(result.primitives[1].current_a, 0.000165, 1e-15);
    NEAR(result.primitives[2].current_a, 0.000165, 1e-15);
    CHECK(result.primitives[1].component_index == 2);
    CHECK(result.primitives[1].terminal_index == 0);
    CHECK(result.max_kcl_residual_a < 1e-15);

    /* kohm, Mohm and mV scale to identical SI answers. */
    vp[0] = mk_param("voltage", 3300, EN_ADPT_UNIT_MV);
    r1p[0] = mk_param("resistance", 10, EN_ADPT_UNIT_KOHM);
    r2p[0] = mk_param("resistance", 0.01, EN_ADPT_UNIT_MOHM);
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[2].voltage_v, 1.65, 1e-12);
    NEAR(result.primitives[1].current_a, 0.000165, 1e-15);
}

static void potentiometer_positions_and_merges(void)
{
    EnAdptResult result;
    EnAdptGraph graph;
    EnAdptTerminal gt[1], vt[2], pt[3], st[2], tt[2];
    EnAdptParameter vp[1], pp[2], sp[1], tp[1];
    EnAdptComponent components[5];
    EnAdptId gnd_eps[4], vplus_eps[2], mid_eps[2], x_eps[2];
    EnAdptNet nets[4];
    const double total = 10000.0;

    gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                    EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[0] = mk_term("VS.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[1] = mk_term("VS.n", "n", EN_ADPT_DOMAIN_GROUND, EN_ADPT_DIRECTION_UNSPECIFIED);
    pt[0] = mk_term("RP.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    pt[1] = mk_term("RP.w", "w", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    pt[2] = mk_term("RP.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    st[0] = mk_term("RS.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    st[1] = mk_term("RS.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    tt[0] = mk_term("RT.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    tt[1] = mk_term("RT.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
    pp[0] = mk_param("resistance", total, EN_ADPT_UNIT_OHM);
    pp[1] = mk_param("position", 0.5, EN_ADPT_UNIT_RATIO);
    sp[0] = mk_param("resistance", 10000, EN_ADPT_UNIT_OHM);
    tp[0] = mk_param("resistance", 10000, EN_ADPT_UNIT_OHM);
    components[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
    components[1] = mk_comp("VS", EN_ADPT_KIND_VOLTAGE_SOURCE, vt, 2, vp, 1);
    components[2] = mk_comp("RP", EN_ADPT_KIND_POTENTIOMETER, pt, 3, pp, 2);
    components[3] = mk_comp("RS", EN_ADPT_KIND_RESISTOR, st, 2, sp, 1);
    components[4] = mk_comp("RT", EN_ADPT_KIND_RESISTOR, tt, 2, tp, 1);
    gnd_eps[0] = eid("G1.ref");
    gnd_eps[1] = eid("VS.n");
    gnd_eps[2] = eid("RP.b");
    gnd_eps[3] = eid("RT.b");
    vplus_eps[0] = eid("VS.p");
    vplus_eps[1] = eid("RP.a");
    mid_eps[0] = eid("RP.w");
    mid_eps[1] = eid("RS.a");
    x_eps[0] = eid("RS.b");
    x_eps[1] = eid("RT.a");
    nets[0] = mk_net("GND", gnd_eps, 4);
    nets[1] = mk_net("VPLUS", vplus_eps, 2);
    nets[2] = mk_net("MID", mid_eps, 2);
    nets[3] = mk_net("X", x_eps, 2);
    graph = mk_graph(components, 5, nets, 4);

    /* Mid position, loaded by a 20 k divider chain: V_MID = 22/15 V and
     * V_X = 11/15 V. Primitives: source + two segments + RS + RT. */
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[2].voltage_v, 22.0 / 15.0, 1e-12);
    NEAR(result.nets[3].voltage_v, 11.0 / 15.0, 1e-12);
    CHECK(result.primitive_count == 5);
    NEAR(result.primitives[1].current_a, (3.3 - 22.0 / 15.0) / (total * 0.5),
         1e-15);
    NEAR(result.primitives[2].current_a, (22.0 / 15.0) / (total * 0.5), 1e-15);
    NEAR(result.primitives[3].current_a, (22.0 / 15.0) / 20000.0, 1e-15);

    /* Percent unit is equivalent to ratio. */
    pp[1] = mk_param("position", 50, EN_ADPT_UNIT_PERCENT);
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[2].voltage_v, 22.0 / 15.0, 1e-12);

    /* Exact 0 merges the wiper with terminal a: V_MID = 3.3 and the chain
     * divides it to 1.65. Only one segment primitive remains. */
    pp[1] = mk_param("position", 0, EN_ADPT_UNIT_RATIO);
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[2].voltage_v, 3.3, 1e-12);
    NEAR(result.nets[3].voltage_v, 1.65, 1e-12);
    CHECK(result.primitive_count == 4);
    NEAR(result.terminals[4].voltage_v, 3.3, 1e-12); /* RP.w equals RP.a */

    /* Exact 1 merges the wiper with terminal b: V_MID = 0 (reference). */
    pp[1] = mk_param("position", 1, EN_ADPT_UNIT_RATIO);
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    CHECK(result.nets[2].voltage_v == 0.0);
    CHECK(result.nets[3].voltage_v == 0.0);
    CHECK(result.primitive_count == 4);

    /* Out-of-range or ill-typed positions fail explicitly. */
    pp[1] = mk_param("position", 1.5, EN_ADPT_UNIT_RATIO);
    expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "position", &result);
    pp[1] = mk_param("position", -1, EN_ADPT_UNIT_PERCENT);
    expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "position", &result);
    pp[1] = mk_param("position", 50, EN_ADPT_UNIT_V);
    expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "position", &result);
    pp[1] = mk_param("position", NAN, EN_ADPT_UNIT_RATIO);
    expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "position", &result);
}

static void source_series_resistance_forms(void)
{
    EnAdptResult result;
    EnAdptGraph graph;
    EnAdptTerminal gt[1], vt[2], lt[2], it[2];
    EnAdptParameter vp[2], lp[1], ip[2];
    EnAdptComponent components[3];
    EnAdptId gnd_eps[3], out_eps[2];
    EnAdptNet nets[2];

    gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                    EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[0] = mk_term("VS.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[1] = mk_term("VS.n", "n", EN_ADPT_DOMAIN_GROUND, EN_ADPT_DIRECTION_UNSPECIFIED);
    lt[0] = mk_term("RL.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    lt[1] = mk_term("RL.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    /* Thevenin source: 3.3 V behind 10 ohm, loaded with 990 ohm. */
    vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
    vp[1] = mk_param("series_resistance", 10, EN_ADPT_UNIT_OHM);
    lp[0] = mk_param("resistance", 990, EN_ADPT_UNIT_OHM);
    components[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
    components[1] = mk_comp("VS", EN_ADPT_KIND_VOLTAGE_SOURCE, vt, 2, vp, 2);
    components[2] = mk_comp("RL", EN_ADPT_KIND_RESISTOR, lt, 2, lp, 1);
    gnd_eps[0] = eid("G1.ref");
    gnd_eps[1] = eid("VS.n");
    gnd_eps[2] = eid("RL.b");
    out_eps[0] = eid("VS.p");
    out_eps[1] = eid("RL.a");
    nets[0] = mk_net("GND", gnd_eps, 3);
    nets[1] = mk_net("VOUT", out_eps, 2);
    graph = mk_graph(components, 3, nets, 2);

    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[1].voltage_v, 3.3 * 990.0 / 1000.0, 1e-12);
    CHECK(result.primitive_count == 2); /* driver, no ideal constraint */
    CHECK(result.primitives[0].terminal_index == 0);
    NEAR(result.primitives[0].current_a, -0.0033, 1e-15);
    NEAR(result.primitives[1].current_a, 0.0033, 1e-15);

    /* Omitting series_resistance restores the ideal source. */
    components[1].parameter_count = 1;
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[1].voltage_v, 3.3, 1e-12);
    CHECK(result.primitive_count == 2);

    /* Norton source: 2 mA feeding N1 (current exits the n terminal), with
     * a 1 kohm shunt and 1 kohm load -> V_N1 = +1 V. */
    it[0] = mk_term("IS.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    it[1] = mk_term("IS.n", "n", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    ip[0] = mk_param("current", 2, EN_ADPT_UNIT_MA);
    ip[1] = mk_param("series_resistance", 1, EN_ADPT_UNIT_KOHM);
    lp[0] = mk_param("resistance", 1000, EN_ADPT_UNIT_OHM);
    components[1] = mk_comp("IS", EN_ADPT_KIND_CURRENT_SOURCE, it, 2, ip, 2);
    gnd_eps[1] = eid("IS.p");
    nets[0] = mk_net("GND", gnd_eps, 3);
    out_eps[0] = eid("IS.n");
    out_eps[1] = eid("RL.a");
    nets[1] = mk_net("N1", out_eps, 2);
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[1].voltage_v, 1.0, 1e-12);
    CHECK(result.primitive_count == 3); /* source + shunt + load */
    NEAR(result.primitives[0].current_a, 0.002, 1e-15);
    NEAR(result.primitives[1].current_a, -0.001, 1e-15); /* shunt, p on GND */
    NEAR(result.primitives[2].current_a, 0.001, 1e-15);

    /* Ideal mA source: doubled node voltage, no shunt primitive. */
    components[1].parameter_count = 1;
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[1].voltage_v, 2.0, 1e-12);
    CHECK(result.primitive_count == 2);
}

static void switch_states(void)
{
    EnAdptResult result;
    EnAdptGraph graph;
    EnAdptTerminal gt[1], vt[2], r1t[2], st[2], lt[2], u1t[2];
    EnAdptParameter vp[1], r1p[1], sp[3], lp[1];
    EnAdptComponent components[5];
    EnAdptId gnd_eps[3], vplus_eps[2], mid_eps[2], out_eps[2];
    EnAdptNet nets[4];

    gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                    EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[0] = mk_term("VS.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[1] = mk_term("VS.n", "n", EN_ADPT_DOMAIN_GROUND, EN_ADPT_DIRECTION_UNSPECIFIED);
    r1t[0] = mk_term("R1.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    r1t[1] = mk_term("R1.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    st[0] = mk_term("SW.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    st[1] = mk_term("SW.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    lt[0] = mk_term("RL.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    lt[1] = mk_term("RL.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
    r1p[0] = mk_param("resistance", 1000, EN_ADPT_UNIT_OHM);
    sp[0] = mk_param("state", 0, EN_ADPT_UNIT_STATE_CLOSED);
    sp[1] = mk_param("on_resistance", 10, EN_ADPT_UNIT_OHM);
    lp[0] = mk_param("resistance", 1000, EN_ADPT_UNIT_OHM);
    components[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
    components[1] = mk_comp("VS", EN_ADPT_KIND_VOLTAGE_SOURCE, vt, 2, vp, 1);
    components[2] = mk_comp("R1", EN_ADPT_KIND_RESISTOR, r1t, 2, r1p, 1);
    components[3] = mk_comp("SW", EN_ADPT_KIND_SWITCH, st, 2, sp, 2);
    components[4] = mk_comp("RL", EN_ADPT_KIND_RESISTOR, lt, 2, lp, 1);
    gnd_eps[0] = eid("G1.ref");
    gnd_eps[1] = eid("VS.n");
    gnd_eps[2] = eid("RL.b");
    vplus_eps[0] = eid("VS.p");
    vplus_eps[1] = eid("R1.a");
    mid_eps[0] = eid("R1.b");
    mid_eps[1] = eid("SW.a");
    out_eps[0] = eid("SW.b");
    out_eps[1] = eid("RL.a");
    nets[0] = mk_net("GND", gnd_eps, 3);
    nets[1] = mk_net("VPLUS", vplus_eps, 2);
    nets[2] = mk_net("MID", mid_eps, 2);
    nets[3] = mk_net("OUT", out_eps, 2);
    graph = mk_graph(components, 5, nets, 4);

    /* Closed through 10 ohm: V_OUT = 3.3 * 1000 / 2010. */
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[3].voltage_v, 3.3 * 1000.0 / 2010.0, 1e-12);
    CHECK(result.primitive_count == 4);

    /* Open with explicit 100 Mohm off_resistance: leakage divider. */
    sp[0] = mk_param("state", 0, EN_ADPT_UNIT_STATE_OPEN);
    sp[1] = mk_param("off_resistance", 100, EN_ADPT_UNIT_MOHM);
    components[3].parameters = sp;
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[3].voltage_v, 3.3 * 1000.0 / 100002000.0, 1e-11);
    CHECK(result.primitive_count == 4);

    /* Open without off_resistance: the switch emits no element. RL still
     * ties OUT to the reference, so OUT solves to exactly 0 V. */
    components[3].parameter_count = 1;
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    CHECK(result.nets[3].voltage_v == 0.0);
    CHECK(result.primitive_count == 3); /* VS, R1, RL: no switch element */

    /* Closed without on_resistance: no default resistance is invented. */
    sp[0] = mk_param("state", 0, EN_ADPT_UNIT_STATE_CLOSED);
    components[3].parameters = sp;
    expect_fail(&graph, EN_ADPT_UNSUPPORTED, "SW", &result);

    /* Missing state parameter. */
    components[3].parameter_count = 0;
    expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "state", &result);

    /* Open with nothing else on OUT: the MCU input pad truly floats. RL is
     * replaced by the MCU, so the ground net loses its RL.b endpoint. */
    sp[0] = mk_param("state", 0, EN_ADPT_UNIT_STATE_OPEN);
    components[3].parameters = sp;
    components[3].parameter_count = 1;
    u1t[0] = mk_term("U1.GPIO5", "io5", EN_ADPT_DOMAIN_DIGITAL,
                     EN_ADPT_DIRECTION_INPUT);
    components[4] = mk_comp("U1", EN_ADPT_KIND_MCU, u1t, 1, NULL, 0);
    out_eps[1] = eid("U1.GPIO5");
    gnd_eps[2] = eid("VS.n");
    nets[0] = mk_net("GND", gnd_eps, 2);
    CHECK_SOLVE(&graph, EN_ADPT_FLOATING, &result);
    CHECK(isnan(result.nets[3].voltage_v));
    CHECK(result.nets[3].floating == 1);
    NEAR(result.nets[2].voltage_v, 3.3, 1e-12); /* MID stays driven */
}

static void mcu_open_drain_pull_release_and_contention(void)
{
    EnAdptResult result;
    EnAdptGraph graph;
    EnAdptTerminal gt[1], vt[2], put[2], u1t[3], u2t[2];
    EnAdptParameter vp[1], pup[1];
    EnAdptComponent components[4];
    EnAdptId gnd_eps[3], vdd_eps[2], sig_eps[3];
    EnAdptNet nets[3];
    double asserted;

    gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                    EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[0] = mk_term("VS.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[1] = mk_term("VS.n", "n", EN_ADPT_DOMAIN_GROUND, EN_ADPT_DIRECTION_UNSPECIFIED);
    put[0] = mk_term("RPU.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    put[1] = mk_term("RPU.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
    pup[0] = mk_param("resistance", 45000, EN_ADPT_UNIT_OHM);
    u1t[0] = mk_term("U1.GND", "gnd", EN_ADPT_DOMAIN_GROUND,
                     EN_ADPT_DIRECTION_UNSPECIFIED);
    u1t[1] = mk_term("U1.GPIO4", "io4", EN_ADPT_DOMAIN_DIGITAL,
                     EN_ADPT_DIRECTION_INOUT);
    u1t[1].driver.drive = EN_ADPT_DRIVE_OPEN_DRAIN;
    u1t[1].driver.voltage_v = 0.0;
    u1t[1].driver.impedance_ohm = 20.0;
    u1t[1].driver.return_id = "U1.GND";
    u1t[2] = mk_term("U1.GPIO5", "io5", EN_ADPT_DOMAIN_DIGITAL,
                     EN_ADPT_DIRECTION_INPUT);
    components[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
    components[1] = mk_comp("U1", EN_ADPT_KIND_MCU, u1t, 3, NULL, 0);
    components[2] = mk_comp("VS", EN_ADPT_KIND_VOLTAGE_SOURCE, vt, 2, vp, 1);
    components[3] = mk_comp("RPU", EN_ADPT_KIND_RESISTOR, put, 2, pup, 1);
    gnd_eps[0] = eid("G1.ref");
    gnd_eps[1] = eid("VS.n");
    gnd_eps[2] = eid("U1.GND");
    vdd_eps[0] = eid("VS.p");
    vdd_eps[1] = eid("RPU.a");
    sig_eps[0] = eid("U1.GPIO4");
    sig_eps[1] = eid("RPU.b");
    sig_eps[2] = eid("U1.GPIO5");
    nets[0] = mk_net("GND", gnd_eps, 3);
    nets[1] = mk_net("VDD", vdd_eps, 2);
    nets[2] = mk_net("SIG", sig_eps, 3);
    graph = mk_graph(components, 4, nets, 3);

    /* Asserted open-drain low against an explicit 45 k pull. Primitives in
     * component order: U1 driver, VS source, RPU. */
    asserted = 3.3 * 20.0 / 45020.0;
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[2].voltage_v, asserted, 1e-15);
    NEAR(result.terminals[2].voltage_v, asserted, 1e-15); /* U1.GPIO4 */
    CHECK(result.primitive_count == 3);
    NEAR(result.primitives[0].current_a, asserted / 20.0, 1e-16);
    NEAR(result.primitives[2].current_a, (3.3 - asserted) / 45000.0, 1e-16);

    /* Release omits the low driver entirely; no high driver is inserted and
     * the node rises to the pull rail with no pull current. */
    u1t[1].driver.drive = EN_ADPT_DRIVE_NONE;
    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    NEAR(result.nets[2].voltage_v, 3.3, 1e-12);
    CHECK(result.primitive_count == 2); /* VS, RPU only */
    NEAR(result.primitives[1].current_a, 0.0, 1e-15);
    u1t[1].driver.drive = EN_ADPT_DRIVE_OPEN_DRAIN;

    /* Push-pull contention: two explicit 40 ohm drivers oppose. */
    u1t[1].driver.drive = EN_ADPT_DRIVE_PUSH_PULL;
    u1t[1].driver.voltage_v = 3.3;
    u1t[1].driver.impedance_ohm = 40.0;
    u2t[0] = mk_term("U2.GND", "gnd", EN_ADPT_DOMAIN_GROUND,
                     EN_ADPT_DIRECTION_UNSPECIFIED);
    u2t[1] = mk_term("U2.GPIO4", "io4", EN_ADPT_DOMAIN_DIGITAL,
                     EN_ADPT_DIRECTION_INOUT);
    u2t[1].driver.drive = EN_ADPT_DRIVE_PUSH_PULL;
    u2t[1].driver.voltage_v = 0.0;
    u2t[1].driver.impedance_ohm = 40.0;
    u2t[1].driver.return_id = "U2.GND";
    {
        EnAdptComponent contended[5];
        EnAdptId contended_gnd[4], contended_sig[4];
        EnAdptNet contended_nets[3];
        contended[0] = components[0];
        contended[1] = components[1];
        contended[2] = mk_comp("U2", EN_ADPT_KIND_MCU, u2t, 2, NULL, 0);
        contended[3] = components[2];
        contended[4] = components[3];
        contended_gnd[0] = eid("G1.ref");
        contended_gnd[1] = eid("VS.n");
        contended_gnd[2] = eid("U1.GND");
        contended_gnd[3] = eid("U2.GND");
        contended_sig[0] = eid("U1.GPIO4");
        contended_sig[1] = eid("RPU.b");
        contended_sig[2] = eid("U1.GPIO5");
        contended_sig[3] = eid("U2.GPIO4");
        contended_nets[0] = mk_net("GND", contended_gnd, 4);
        contended_nets[1] = nets[1];
        contended_nets[2] = mk_net("SIG", contended_sig, 4);
        graph = mk_graph(contended, 5, contended_nets, 3);
        CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
        {
            /* Both 40 ohm drivers plus the 45 k pull to the rail. */
            double v = (3.3 / 40.0 + 3.3 / 45000.0) /
                       (2.0 / 40.0 + 1.0 / 45000.0);
            NEAR(result.nets[2].voltage_v, v, 1e-12);
            CHECK(result.primitive_count == 4);
            NEAR(result.primitives[0].current_a, (v - 3.3) / 40.0, 1e-14);
            NEAR(result.primitives[1].current_a, v / 40.0, 1e-14);
        }
    }
    u1t[1].driver.drive = EN_ADPT_DRIVE_OPEN_DRAIN;
    u1t[1].driver.voltage_v = 0.0;
    graph = mk_graph(components, 4, nets, 3);

    /* Driver errors: every one explicit, naming the offender. */
    u1t[1].driver.return_id = NULL;
    expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "U1.GPIO4", &result);
    u1t[1].driver.return_id = "U1.VDDX";
    expect_fail(&graph, EN_ADPT_UNKNOWN_ENDPOINT, "U1.VDDX", &result);
    u1t[1].driver.return_id = "U1.GND";
    u1t[1].driver.impedance_ohm = 0.0;
    expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "U1.GPIO4", &result);
    u1t[1].driver.impedance_ohm = 20.0;
    u1t[2].driver.drive = EN_ADPT_DRIVE_PUSH_PULL;
    u1t[2].driver.voltage_v = 0.0;
    u1t[2].driver.impedance_ohm = 20.0;
    u1t[2].driver.return_id = "U1.GND";
    expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "U1.GPIO5", &result);
    u1t[2].driver.drive = EN_ADPT_DRIVE_NONE;

    /* A driver on a non-MCU terminal is unsupported. */
    put[0].driver.drive = EN_ADPT_DRIVE_PUSH_PULL;
    put[0].driver.voltage_v = 3.3;
    put[0].driver.impedance_ohm = 50.0;
    put[0].driver.return_id = "U1.GND";
    expect_fail(&graph, EN_ADPT_UNSUPPORTED, "RPU", &result);
    put[0].driver.drive = EN_ADPT_DRIVE_NONE;

    /* Driven pad on the same net as its return: shorted, not representable. */
    {
        EnAdptId shorted_gnd[4];
        EnAdptNet shorted_nets[1];
        EnAdptComponent shorted_comps[2];
        shorted_gnd[0] = eid("G1.ref");
        shorted_gnd[1] = eid("U1.GND");
        shorted_gnd[2] = eid("U1.GPIO4");
        shorted_gnd[3] = eid("U1.GPIO5");
        shorted_nets[0] = mk_net("GND", shorted_gnd, 4);
        shorted_comps[0] = components[0];
        shorted_comps[1] = components[1];
        graph = mk_graph(shorted_comps, 2, shorted_nets, 1);
        expect_fail(&graph, EN_ADPT_UNSUPPORTED, "U1", &result);
    }
}

static void floating_and_no_ground(void)
{
    EnAdptResult result;
    EnAdptGraph graph;
    EnAdptTerminal gt[1], vt[2], r1t[2], r2t[2], u1t[1], u2t[1];
    EnAdptParameter vp[1], r1p[1], r2p[1];
    EnAdptComponent components[6];
    EnAdptId gnd_eps[3], vplus_eps[2], mid_eps[2], iso_eps[2];
    EnAdptNet nets[4];

    /* (a) An isolated net between two input pads floats while a grounded
     * divider on the same graph solves normally. */
    gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                    EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[0] = mk_term("VS.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[1] = mk_term("VS.n", "n", EN_ADPT_DOMAIN_GROUND, EN_ADPT_DIRECTION_UNSPECIFIED);
    r1t[0] = mk_term("R1.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    r1t[1] = mk_term("R1.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    r2t[0] = mk_term("R2.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    r2t[1] = mk_term("R2.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
    r1p[0] = mk_param("resistance", 10000, EN_ADPT_UNIT_OHM);
    r2p[0] = mk_param("resistance", 10000, EN_ADPT_UNIT_OHM);
    u1t[0] = mk_term("U1.GPIO5", "io5", EN_ADPT_DOMAIN_DIGITAL,
                     EN_ADPT_DIRECTION_INPUT);
    u2t[0] = mk_term("U2.GPIO5", "io5", EN_ADPT_DOMAIN_DIGITAL,
                     EN_ADPT_DIRECTION_INPUT);
    components[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
    components[1] = mk_comp("VS", EN_ADPT_KIND_VOLTAGE_SOURCE, vt, 2, vp, 1);
    components[2] = mk_comp("R1", EN_ADPT_KIND_RESISTOR, r1t, 2, r1p, 1);
    components[3] = mk_comp("R2", EN_ADPT_KIND_RESISTOR, r2t, 2, r2p, 1);
    components[4] = mk_comp("U1", EN_ADPT_KIND_MCU, u1t, 1, NULL, 0);
    components[5] = mk_comp("U2", EN_ADPT_KIND_MCU, u2t, 1, NULL, 0);
    gnd_eps[0] = eid("G1.ref");
    gnd_eps[1] = eid("VS.n");
    gnd_eps[2] = eid("R2.b");
    vplus_eps[0] = eid("VS.p");
    vplus_eps[1] = eid("R1.a");
    mid_eps[0] = eid("R1.b");
    mid_eps[1] = eid("R2.a");
    iso_eps[0] = eid("U1.GPIO5");
    iso_eps[1] = eid("U2.GPIO5");
    nets[0] = mk_net("GND", gnd_eps, 3);
    nets[1] = mk_net("VPLUS", vplus_eps, 2);
    nets[2] = mk_net("MID", mid_eps, 2);
    nets[3] = mk_net("ISO", iso_eps, 2);
    graph = mk_graph(components, 6, nets, 4);

    CHECK_SOLVE(&graph, EN_ADPT_FLOATING, &result);
    NEAR(result.nets[2].voltage_v, 1.65, 1e-12);
    CHECK(isnan(result.nets[3].voltage_v));
    CHECK(result.nets[3].floating == 1);
    CHECK(isnan(result.terminals[7].voltage_v)); /* U1.GPIO5 */
    CHECK(result.terminals[7].floating == 1);
    NEAR(result.primitives[1].current_a, 0.000165, 1e-15);

    /* (b) No ground anywhere: relative currents solve, absolute voltages
     * stay NaN. Nothing is fabricated to 0 V. */
    {
        EnAdptComponent no_gnd[3];
        EnAdptId a_eps[2], m_eps[2], b_eps[2];
        EnAdptNet no_gnd_nets[3];
        no_gnd[0] = components[1];
        no_gnd[1] = components[2];
        no_gnd[2] = components[3];
        vp[0] = mk_param("voltage", 5, EN_ADPT_UNIT_V);
        a_eps[0] = eid("VS.p");
        a_eps[1] = eid("R1.a");
        m_eps[0] = eid("R1.b");
        m_eps[1] = eid("R2.a");
        b_eps[0] = eid("VS.n");
        b_eps[1] = eid("R2.b");
        no_gnd_nets[0] = mk_net("A", a_eps, 2);
        no_gnd_nets[1] = mk_net("M", m_eps, 2);
        no_gnd_nets[2] = mk_net("B", b_eps, 2);
        graph = mk_graph(no_gnd, 3, no_gnd_nets, 3);
        CHECK_SOLVE(&graph, EN_ADPT_FLOATING, &result);
        CHECK(isnan(result.nets[0].voltage_v));
        CHECK(isnan(result.nets[1].voltage_v));
        CHECK(isnan(result.nets[2].voltage_v));
        CHECK(result.nets[0].is_reference == 0);
        CHECK(result.node_count == 4); /* phantom node 0 + A, M, B */
        NEAR(result.primitives[0].current_a, -0.00025, 1e-15);
        NEAR(result.primitives[1].current_a, 0.00025, 1e-15);
        NEAR(result.primitives[2].current_a, 0.00025, 1e-15);
    }

    /* (c) An unwired resistor terminal is its own node. The branch carries
     * zero current, not a graph error or an invented wire to reference. */
    {
        EnAdptId gnd2[2], vplus2[2];
        EnAdptNet partial[2];
        result.nets[0].voltage_v = 123.0; /* stale bytes must not survive */
        gnd2[0] = eid("G1.ref");
        gnd2[1] = eid("VS.n");
        vplus2[0] = eid("VS.p");
        vplus2[1] = eid("R1.a");
        partial[0] = mk_net("GND", gnd2, 2);
        partial[1] = mk_net("VPLUS", vplus2, 2);
        graph = mk_graph(components, 4, partial, 2);
        CHECK_SOLVE(&graph, EN_ADPT_FLOATING, &result);
        NEAR(result.nets[0].voltage_v, 0.0, 1e-12);
        NEAR(result.nets[1].voltage_v, 5.0, 1e-12);
        CHECK(!result.terminals[4].on_net); /* R1.b */
        NEAR(result.terminals[4].voltage_v, 5.0, 1e-12);
        NEAR(result.primitives[1].current_a, 0.0, 1e-15);
        CHECK(isnan(result.terminals[5].voltage_v)); /* isolated R2 */
        CHECK(isnan(result.terminals[6].voltage_v));
    }
}

static void unsupported_and_parameter_failures(void)
{
    EnAdptResult result;
    EnAdptGraph graph;
    EnAdptTerminal gt[1], vt[2], ct[2], dt[2];
    EnAdptParameter vp[1];
    EnAdptComponent components[3];
    EnAdptId gnd_eps[3], va_eps[2];
    EnAdptNet nets[2];

    gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                    EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[0] = mk_term("VS.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[1] = mk_term("VS.n", "n", EN_ADPT_DOMAIN_GROUND, EN_ADPT_DIRECTION_UNSPECIFIED);
    ct[0] = mk_term("C1.p", "p", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    ct[1] = mk_term("C1.n", "n", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
    components[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
    components[1] = mk_comp("VS", EN_ADPT_KIND_VOLTAGE_SOURCE, vt, 2, vp, 1);
    components[2] = mk_comp("C1", EN_ADPT_KIND_CAPACITOR, ct, 2, NULL, 0);
    gnd_eps[0] = eid("G1.ref");
    gnd_eps[1] = eid("VS.n");
    gnd_eps[2] = eid("C1.n");
    va_eps[0] = eid("VS.p");
    va_eps[1] = eid("C1.p");
    nets[0] = mk_net("GND", gnd_eps, 3);
    nets[1] = mk_net("VA", va_eps, 2);
    graph = mk_graph(components, 3, nets, 2);

    /* A capacitor has no DC primitive and is never silently dropped. */
    expect_fail(&graph, EN_ADPT_UNSUPPORTED, "C1", &result);

    /* Generic devices have no defined DC primitive either. */
    dt[0] = mk_term("D1.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    dt[1] = mk_term("D1.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    components[2] = mk_comp("D1", EN_ADPT_KIND_DEVICE, dt, 2, NULL, 0);
    gnd_eps[2] = eid("D1.b");
    va_eps[1] = eid("D1.a");
    nets[0] = mk_net("GND", gnd_eps, 3);
    nets[1] = mk_net("VA", va_eps, 2);
    expect_fail(&graph, EN_ADPT_UNSUPPORTED, "D1", &result);
    gnd_eps[2] = eid("C1.n");
    va_eps[1] = eid("C1.p");
    components[2] = mk_comp("C1", EN_ADPT_KIND_CAPACITOR, ct, 2, NULL, 0);

    /* Resistor parameter failures. */
    {
        EnAdptTerminal rt[2];
        EnAdptParameter rp[1];
        EnAdptComponent rgraph[3];
        rt[0] = mk_term("R1.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
        rt[1] = mk_term("R1.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
        rgraph[0] = components[0];
        rgraph[1] = components[1];
        va_eps[1] = eid("R1.a");
        gnd_eps[2] = eid("R1.b");
        nets[1] = mk_net("VA", va_eps, 2);
        nets[0] = mk_net("GND", gnd_eps, 3);
        rgraph[2] = mk_comp("R1", EN_ADPT_KIND_RESISTOR, rt, 2, rp, 1);
        graph = mk_graph(rgraph, 3, nets, 2);

        rp[0] = mk_param("temperature_coefficient", 100, EN_ADPT_UNIT_RATIO);
        expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "temperature_coefficient",
                    &result);
        rgraph[2].parameter_count = 0;
        expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "resistance", &result);
        rp[0] = mk_param("resistance", 1000, EN_ADPT_UNIT_V);
        rgraph[2].parameters = rp;
        rgraph[2].parameter_count = 1;
        expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "resistance", &result);
        rp[0] = mk_param("resistance", 0, EN_ADPT_UNIT_OHM);
        expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "resistance", &result);
        rp[0] = mk_param("resistance", -5, EN_ADPT_UNIT_KOHM);
        expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "resistance", &result);
        rp[0] = mk_param("resistance", INFINITY, EN_ADPT_UNIT_OHM);
        expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "resistance", &result);
        rp[0] = mk_param("resistance", 1000, EN_ADPT_UNIT_OHM);

        /* Voltage source failures: NaN value and zero series resistance. */
        vp[0] = mk_param("voltage", NAN, EN_ADPT_UNIT_V);
        expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "voltage", &result);
        vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
        {
            EnAdptParameter vser[2];
            vser[0] = vp[0];
            vser[1] = mk_param("series_resistance", 0, EN_ADPT_UNIT_OHM);
            rgraph[1].parameters = vser;
            rgraph[1].parameter_count = 2;
            expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "series_resistance", &result);
            rgraph[1].parameters = vp;
            rgraph[1].parameter_count = 1;
        }

        /* Ground carrying parameters has no defined meaning. */
        {
            EnAdptParameter gp[1];
            EnAdptComponent gwith[1];
            gp[0] = mk_param("nominal", 0, EN_ADPT_UNIT_V);
            gwith[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, gp, 1);
            rgraph[0] = gwith[0];
            expect_fail(&graph, EN_ADPT_INVALID_PARAMETER, "nominal", &result);
        }
    }

    /* Same-net shorts and ideal-source conflicts are covered by
     * same_net_shorts_and_source_conflicts() below. */
}

static void same_net_shorts_and_source_conflicts(void)
{
    EnAdptResult result;
    EnAdptGraph graph;
    EnAdptTerminal gt[1], vt[2], v2t[2];
    EnAdptParameter vp[1], v2p[1];
    EnAdptComponent components[3];

    gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                    EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[0] = mk_term("VS.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[1] = mk_term("VS.n", "n", EN_ADPT_DOMAIN_GROUND, EN_ADPT_DIRECTION_UNSPECIFIED);
    v2t[0] = mk_term("VS2.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    v2t[1] = mk_term("VS2.n", "n", EN_ADPT_DOMAIN_GROUND, EN_ADPT_DIRECTION_UNSPECIFIED);
    vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
    v2p[0] = mk_param("voltage", 5.0, EN_ADPT_UNIT_V);
    components[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
    components[1] = mk_comp("VS", EN_ADPT_KIND_VOLTAGE_SOURCE, vt, 2, vp, 1);
    components[2] = mk_comp("VS2", EN_ADPT_KIND_VOLTAGE_SOURCE, v2t, 2, v2p, 1);

    /* An element whose two terminals land on one net is shorted and not
     * representable; the kernel would reject it as an invalid element. */
    {
        EnAdptTerminal rt[2], rt2[2];
        EnAdptParameter rp[1];
        EnAdptComponent pair[2];
        EnAdptId x_eps[2], y_eps[2];
        EnAdptNet pair_nets[2];
        rt[0] = mk_term("R1.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
        rt[1] = mk_term("R1.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
        rt2[0] = mk_term("R2.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
        rt2[1] = mk_term("R2.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
        rp[0] = mk_param("resistance", 1000, EN_ADPT_UNIT_OHM);
        pair[0] = mk_comp("R1", EN_ADPT_KIND_RESISTOR, rt, 2, rp, 1);
        pair[1] = mk_comp("R2", EN_ADPT_KIND_RESISTOR, rt2, 2, rp, 1);
        x_eps[0] = eid("R1.a");
        x_eps[1] = eid("R1.b");
        y_eps[0] = eid("R2.a");
        y_eps[1] = eid("R2.b");
        pair_nets[0] = mk_net("X", x_eps, 2);
        pair_nets[1] = mk_net("Y", y_eps, 2);
        graph = mk_graph(pair, 2, pair_nets, 2);
        expect_fail(&graph, EN_ADPT_UNSUPPORTED, "R1", &result);
    }

    /* Endpoint merging can short a primitive whose first terminal owns a
     * private group. Error reporting must not index the declared-net array
     * with that private index or leave successful voltages behind. */
    {
        EnAdptTerminal pot[3] = {
            mk_term("P.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE),
            mk_term("P.w", "w", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE),
            mk_term("P.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE)
        };
        EnAdptParameter parameters[2] = {
            mk_param("resistance", 5000, EN_ADPT_UNIT_OHM),
            mk_param("position", 0, EN_ADPT_UNIT_RATIO)
        };
        EnAdptComponent component = mk_comp("P", EN_ADPT_KIND_POTENTIOMETER,
                                          pot, 3, parameters, 2);
        EnAdptId endpoints[2] = {eid("P.w"), eid("P.b")};
        EnAdptNet net = mk_net("joined", endpoints, 2);
        graph = mk_graph(&component, 1, &net, 1);
        expect_fail(&graph, EN_ADPT_UNSUPPORTED, "P", &result);
        CHECK(result.bad_component == 0);
    }

    /* Conflicting ideal sources across two nets: singular, never averaged. */
    {
        EnAdptId a_eps[2], gnd_eps[3];
        EnAdptNet pair_nets[2];
        a_eps[0] = eid("VS.p");
        a_eps[1] = eid("VS2.p");
        gnd_eps[0] = eid("G1.ref");
        gnd_eps[1] = eid("VS.n");
        gnd_eps[2] = eid("VS2.n");
        pair_nets[0] = mk_net("A", a_eps, 2);
        pair_nets[1] = mk_net("GND", gnd_eps, 3);
        graph = mk_graph(components, 3, pair_nets, 2);
        CHECK_SOLVE(&graph, EN_ADPT_SINGULAR, &result);
        CHECK(isnan(result.nets[0].voltage_v));
        CHECK(result.bad_component == SIZE_MAX);

        /* Equal-valued redundant sources stay singular: the source currents
         * are ambiguous even when the voltages agree. */
        v2p[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
        CHECK_SOLVE(&graph, EN_ADPT_SINGULAR, &result);
    }
}

static void limits(void)
{
    EnAdptResult result;
    EnAdptGraph graph;
    static EnAdptTerminal pads[128];
    static EnAdptComponent mcus[8];
    static EnAdptNet nets[64];
    static EnAdptId net_eps[64][2];
    EnAdptId eps2[2];
    static EnAdptNet more[64];
    char name[EN_ADPT_ID_MAX + 1];

    /* 63 separate pad-pair nets fit exactly: 63 non-reference nodes plus the
     * reference make the full 64-node bound. All nodes are element-free
     * islands, so the honest answer is FLOATING with NaN everywhere. */
    for (size_t u = 0; u < 8; ++u) {
        for (size_t t = 0; t < 16; ++t) {
            snprintf(name, sizeof(name), "U%zu.IN%zu", u + 1, t);
            pads[u * 16 + t] = mk_term(name, "in", EN_ADPT_DOMAIN_DIGITAL,
                                       EN_ADPT_DIRECTION_INPUT);
        }
        snprintf(name, sizeof(name), "U%zu", u + 1);
        mcus[u] = mk_comp(name, EN_ADPT_KIND_MCU, &pads[u * 16], 16, NULL, 0);
    }
    for (size_t n = 0; n < 63; ++n) {
        snprintf(name, sizeof(name), "N%zu", n);
        net_eps[n][0] = eid(pads[2 * n].id.text);
        net_eps[n][1] = eid(pads[2 * n + 1].id.text);
        nets[n] = mk_net(name, net_eps[n], 2);
    }
    graph = mk_graph(mcus, 8, nets, 63);
    CHECK_SOLVE(&graph, EN_ADPT_FLOATING, &result);
    CHECK(result.node_count == 64);

    /* One more distinct net exceeds the bound, naming the offending net. */
    memcpy(more, nets, 63 * sizeof(EnAdptNet));
    eps2[0] = eid(pads[126].id.text);
    eps2[1] = eid(pads[127].id.text);
    more[63] = mk_net("N63", eps2, 2);
    graph = mk_graph(mcus, 8, more, 64);
    expect_fail(&graph, EN_ADPT_EXCEEDS_LIMITS, "N63", &result);

    /* Potentiometer segments cross the 128-primitive bound. */
    {
        static EnAdptTerminal pts[3 * 85];
        static EnAdptParameter pot_params[2 * 85];
        static EnAdptComponent pots[85];
        static EnAdptId gnd_eps[2 + 85], vplus_eps[1 + 85], mid_eps[85];
        static EnAdptNet pot_nets[3];
        static EnAdptComponent all[2 + 85];
        EnAdptTerminal gt[1], vt[2];
        EnAdptParameter vp[1];
        size_t count;

        for (size_t k = 0; k < 85; ++k) {
            snprintf(name, sizeof(name), "P%02zu.a", k);
            pts[3 * k] = mk_term(name, "a", EN_ADPT_DOMAIN_PASSIVE,
                                 EN_ADPT_DIRECTION_PASSIVE);
            snprintf(name, sizeof(name), "P%02zu.w", k);
            pts[3 * k + 1] = mk_term(name, "w", EN_ADPT_DOMAIN_PASSIVE,
                                     EN_ADPT_DIRECTION_PASSIVE);
            snprintf(name, sizeof(name), "P%02zu.b", k);
            pts[3 * k + 2] = mk_term(name, "b", EN_ADPT_DOMAIN_PASSIVE,
                                     EN_ADPT_DIRECTION_PASSIVE);
            pot_params[2 * k] = mk_param("resistance", 10000, EN_ADPT_UNIT_OHM);
            pot_params[2 * k + 1] = mk_param("position", 0.5, EN_ADPT_UNIT_RATIO);
            snprintf(name, sizeof(name), "P%02zu", k);
            pots[k] = mk_comp(name, EN_ADPT_KIND_POTENTIOMETER, &pts[3 * k], 3,
                              &pot_params[2 * k], 2);
            gnd_eps[2 + k] = eid(pts[3 * k + 2].id.text);
            vplus_eps[1 + k] = eid(pts[3 * k].id.text);
            mid_eps[k] = eid(pts[3 * k + 1].id.text);
        }
        gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                        EN_ADPT_DIRECTION_UNSPECIFIED);
        vt[0] = mk_term("VS.p", "p", EN_ADPT_DOMAIN_POWER,
                        EN_ADPT_DIRECTION_UNSPECIFIED);
        vt[1] = mk_term("VS.n", "n", EN_ADPT_DOMAIN_GROUND,
                        EN_ADPT_DIRECTION_UNSPECIFIED);
        vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
        all[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
        all[1] = mk_comp("VS", EN_ADPT_KIND_VOLTAGE_SOURCE, vt, 2, vp, 1);
        gnd_eps[0] = eid("G1.ref");
        gnd_eps[1] = eid("VS.n");
        vplus_eps[0] = eid("VS.p");

        count = 85;
        for (size_t k = 0; k < count; ++k) {
            all[2 + k] = pots[k];
        }
        pot_nets[0] = mk_net("GND", gnd_eps, 2 + count);
        pot_nets[1] = mk_net("VPLUS", vplus_eps, 1 + count);
        pot_nets[2] = mk_net("WIPES", mid_eps, count);
        graph = mk_graph(all, 2 + count, pot_nets, 3);
        expect_fail(&graph, EN_ADPT_EXCEEDS_LIMITS, NULL, &result);

        /* 63 pots project to 127 primitives and stay inside the bound. */
        count = 63;
        for (size_t k = 0; k < count; ++k) {
            all[2 + k] = pots[k];
        }
        pot_nets[0] = mk_net("GND", gnd_eps, 2 + count);
        pot_nets[1] = mk_net("VPLUS", vplus_eps, 1 + count);
        pot_nets[2] = mk_net("WIPES", mid_eps, count);
        graph = mk_graph(all, 2 + count, pot_nets, 3);
        CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
        CHECK(result.primitive_count == 1 + 2 * count);
        CHECK(result.primitive_count == 127);
        CHECK(result.node_count == 3);
    }

    /* Terminal flattening bound: 33 MCUs x 16 pads = 528 > 512. */
    {
        static EnAdptTerminal many[528];
        static EnAdptComponent many_mcus[33];
        for (size_t u = 0; u < 33; ++u) {
            for (size_t t = 0; t < 16; ++t) {
                snprintf(name, sizeof(name), "W%zu.IN%zu", u + 1, t);
                many[u * 16 + t] = mk_term(name, "in", EN_ADPT_DOMAIN_DIGITAL,
                                           EN_ADPT_DIRECTION_INPUT);
            }
            snprintf(name, sizeof(name), "W%zu", u + 1);
            many_mcus[u] = mk_comp(name, EN_ADPT_KIND_MCU, &many[u * 16], 16,
                                   NULL, 0);
        }
        graph = mk_graph(many_mcus, 33, NULL, 0);
        expect_fail(&graph, EN_ADPT_EXCEEDS_LIMITS, NULL, &result);
    }
}

static void determinism_hundred_repeats(void)
{
    EnAdptResult result, again;
    EnAdptGraph graph;
    EnAdptTerminal gt[1], vt[2], r1t[2], r2t[2], put[2], pt[3], it[2], lt[2],
        u1t[3], ft[2];
    EnAdptParameter vp[1], r1p[1], r2p[1], pup[1], pp[2], ip[2], lp[1], fp[1];
    EnAdptComponent components[9];
    EnAdptId gnd_eps[7], vdd_eps[4], mid_eps[2], sig_eps[4], n1_eps[2];
    EnAdptNet nets[5];
    EnAdptComponent reversed[9];
    const double g_up = 1.0 / 45000.0 + 1.0 / 5000.0;
    const double g_down = 1.0 / 5000.0 + 1.0 / 20.0;

    gt[0] = mk_term("G1.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                    EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[0] = mk_term("VS.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    vt[1] = mk_term("VS.n", "n", EN_ADPT_DOMAIN_GROUND, EN_ADPT_DIRECTION_UNSPECIFIED);
    r1t[0] = mk_term("R1.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    r1t[1] = mk_term("R1.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    r2t[0] = mk_term("R2.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    r2t[1] = mk_term("R2.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    put[0] = mk_term("RPU.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    put[1] = mk_term("RPU.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    pt[0] = mk_term("RP.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    pt[1] = mk_term("RP.w", "w", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    pt[2] = mk_term("RP.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    it[0] = mk_term("IS.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    it[1] = mk_term("IS.n", "n", EN_ADPT_DOMAIN_GROUND, EN_ADPT_DIRECTION_UNSPECIFIED);
    lt[0] = mk_term("RL.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    lt[1] = mk_term("RL.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    u1t[0] = mk_term("U1.GND", "gnd", EN_ADPT_DOMAIN_GROUND,
                     EN_ADPT_DIRECTION_UNSPECIFIED);
    u1t[1] = mk_term("U1.GPIO4", "io4", EN_ADPT_DOMAIN_DIGITAL,
                     EN_ADPT_DIRECTION_INOUT);
    u1t[1].driver.drive = EN_ADPT_DRIVE_OPEN_DRAIN;
    u1t[1].driver.voltage_v = 0.0;
    u1t[1].driver.impedance_ohm = 20.0;
    u1t[1].driver.return_id = "U1.GND";
    u1t[2] = mk_term("U1.GPIO5", "io5", EN_ADPT_DOMAIN_DIGITAL,
                     EN_ADPT_DIRECTION_INPUT);
    vp[0] = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
    r1p[0] = mk_param("resistance", 10000, EN_ADPT_UNIT_OHM);
    r2p[0] = mk_param("resistance", 10000, EN_ADPT_UNIT_OHM);
    pup[0] = mk_param("resistance", 45000, EN_ADPT_UNIT_OHM);
    pp[0] = mk_param("resistance", 10000, EN_ADPT_UNIT_OHM);
    pp[1] = mk_param("position", 0.5, EN_ADPT_UNIT_RATIO);
    ip[0] = mk_param("current", 2, EN_ADPT_UNIT_MA);
    ip[1] = mk_param("series_resistance", 1, EN_ADPT_UNIT_KOHM);
    lp[0] = mk_param("resistance", 1000, EN_ADPT_UNIT_OHM);
    components[0] = mk_comp("G1", EN_ADPT_KIND_GROUND, gt, 1, NULL, 0);
    components[1] = mk_comp("VS", EN_ADPT_KIND_VOLTAGE_SOURCE, vt, 2, vp, 1);
    components[2] = mk_comp("R1", EN_ADPT_KIND_RESISTOR, r1t, 2, r1p, 1);
    components[3] = mk_comp("R2", EN_ADPT_KIND_RESISTOR, r2t, 2, r2p, 1);
    components[4] = mk_comp("RPU", EN_ADPT_KIND_RESISTOR, put, 2, pup, 1);
    components[5] = mk_comp("RP", EN_ADPT_KIND_POTENTIOMETER, pt, 3, pp, 2);
    components[6] = mk_comp("IS", EN_ADPT_KIND_CURRENT_SOURCE, it, 2, ip, 2);
    components[7] = mk_comp("RL", EN_ADPT_KIND_RESISTOR, lt, 2, lp, 1);
    components[8] = mk_comp("U1", EN_ADPT_KIND_MCU, u1t, 3, NULL, 0);
    gnd_eps[0] = eid("G1.ref");
    gnd_eps[1] = eid("VS.n");
    gnd_eps[2] = eid("R2.b");
    gnd_eps[3] = eid("RP.b");
    gnd_eps[4] = eid("IS.p");
    gnd_eps[5] = eid("RL.b");
    gnd_eps[6] = eid("U1.GND");
    vdd_eps[0] = eid("VS.p");
    vdd_eps[1] = eid("R1.a");
    vdd_eps[2] = eid("RPU.a");
    vdd_eps[3] = eid("RP.a");
    mid_eps[0] = eid("R1.b");
    mid_eps[1] = eid("R2.a");
    sig_eps[0] = eid("RPU.b");
    sig_eps[1] = eid("RP.w");
    sig_eps[2] = eid("U1.GPIO4");
    sig_eps[3] = eid("U1.GPIO5");
    n1_eps[0] = eid("IS.n");
    n1_eps[1] = eid("RL.a");
    nets[0] = mk_net("GND", gnd_eps, 7);
    nets[1] = mk_net("VDD", vdd_eps, 4);
    nets[2] = mk_net("MID", mid_eps, 2);
    nets[3] = mk_net("SIG", sig_eps, 4);
    nets[4] = mk_net("N1", n1_eps, 2);
    graph = mk_graph(components, 9, nets, 5);

    CHECK_SOLVE(&graph, EN_ADPT_OK, &result);
    CHECK(result.primitive_count == 10);
    NEAR(result.nets[2].voltage_v, 1.65, 1e-12);
    NEAR(result.nets[4].voltage_v, 1.0, 1e-12);
    /* Open-drain low against pull and both pot halves: analytic network. */
    NEAR(result.nets[3].voltage_v, 3.3 * g_up / (g_up + g_down), 1e-15);
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        CHECK(en_adpt_solve(&graph, &again) == EN_ADPT_OK);
        CHECK(memcmp(&result, &again, sizeof(result)) == 0);
    }

    /* Reversing the component order must keep every analytic answer. */
    for (size_t i = 0; i < 9; ++i) {
        reversed[i] = components[8 - i];
    }
    graph = mk_graph(reversed, 9, nets, 5);
    CHECK(en_adpt_solve(&graph, &again) == EN_ADPT_OK);
    for (size_t i = 0; i < 5; ++i) {
        NEAR(again.nets[i].voltage_v, result.nets[i].voltage_v, 1e-12);
    }

    /* Floating results are byte-deterministic too, NaN payloads included. */
    ft[0] = mk_term("F.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_UNSPECIFIED);
    ft[1] = mk_term("F.n", "n", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE);
    fp[0] = mk_param("voltage", 5, EN_ADPT_UNIT_V);
    {
        EnAdptComponent float_comps[3];
        EnAdptId a_eps[2], m_eps[2], b_eps[2];
        EnAdptNet float_nets[3];
        float_comps[0] = mk_comp("F", EN_ADPT_KIND_VOLTAGE_SOURCE, ft, 2, fp, 1);
        float_comps[1] = mk_comp("R1", EN_ADPT_KIND_RESISTOR, r1t, 2, r1p, 1);
        float_comps[2] = mk_comp("R2", EN_ADPT_KIND_RESISTOR, r2t, 2, r2p, 1);
        a_eps[0] = eid("F.p");
        a_eps[1] = eid("R1.a");
        m_eps[0] = eid("R1.b");
        m_eps[1] = eid("R2.a");
        b_eps[0] = eid("F.n");
        b_eps[1] = eid("R2.b");
        float_nets[0] = mk_net("A", a_eps, 2);
        float_nets[1] = mk_net("M", m_eps, 2);
        float_nets[2] = mk_net("B", b_eps, 2);
        graph = mk_graph(float_comps, 3, float_nets, 3);
        CHECK_SOLVE(&graph, EN_ADPT_FLOATING, &result);
        for (unsigned repeat = 0; repeat < 100; ++repeat) {
            CHECK(en_adpt_solve(&graph, &again) == EN_ADPT_FLOATING);
            CHECK(memcmp(&result, &again, sizeof(result)) == 0);
        }
    }
}

static void unfinished_wiring_remains_a_physical_state(void)
{
    EnAdptResult result;
    EnAdptTerminal terminals[2] = {
        mk_term("SW.a", "a", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE),
        mk_term("SW.b", "b", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE)
    };
    EnAdptParameter state = mk_param("state", 0, EN_ADPT_UNIT_STATE_OPEN);
    EnAdptComponent component = mk_comp("SW", EN_ADPT_KIND_SWITCH,
                                      terminals, 2, &state, 1);
    EnAdptGraph graph = mk_graph(&component, 1, NULL, 0);

    /* A dropped, unwired open switch has no current path, not an invalid
     * array index. Its terminal potentials are unknown, never fabricated 0. */
    CHECK_SOLVE(&graph, EN_ADPT_FLOATING, &result);
    CHECK(result.primitive_count == 0);
    CHECK(isnan(result.terminals[0].voltage_v));
    CHECK(isnan(result.terminals[1].voltage_v));

    terminals[0] = mk_term("U.in", "input", EN_ADPT_DOMAIN_DIGITAL,
                          EN_ADPT_DIRECTION_INPUT);
    component = mk_comp("U", EN_ADPT_KIND_MCU, terminals, 1, NULL, 0);
    {
        EnAdptId endpoint = eid("U.in");
        EnAdptNet nets[2] = {
            mk_net("spare", NULL, 0), mk_net("stub", &endpoint, 1)
        };
        graph = mk_graph(&component, 1, nets, 2);
        CHECK_SOLVE(&graph, EN_ADPT_FLOATING, &result);
        CHECK(result.nets[0].floating && isnan(result.nets[0].voltage_v));
        CHECK(result.nets[1].floating && isnan(result.nets[1].voltage_v));
        CHECK(result.terminals[0].on_net);
        CHECK(result.terminals[0].floating && isnan(result.terminals[0].voltage_v));
    }

    /* A separate ground symbol does not wire an isolated voltage source's
     * return to it. Relative voltage exists; both absolute values stay NaN. */
    {
        EnAdptTerminal ground = mk_term("G.ref", "ref", EN_ADPT_DOMAIN_GROUND,
                                       EN_ADPT_DIRECTION_PASSIVE);
        EnAdptTerminal source[2] = {
            mk_term("V.p", "p", EN_ADPT_DOMAIN_POWER, EN_ADPT_DIRECTION_OUTPUT),
            mk_term("V.n", "n", EN_ADPT_DOMAIN_PASSIVE, EN_ADPT_DIRECTION_PASSIVE)
        };
        EnAdptParameter volts = mk_param("voltage", 3.3, EN_ADPT_UNIT_V);
        EnAdptComponent isolated[2] = {
            mk_comp("G", EN_ADPT_KIND_GROUND, &ground, 1, NULL, 0),
            mk_comp("V", EN_ADPT_KIND_VOLTAGE_SOURCE, source, 2, &volts, 1)
        };
        graph = mk_graph(isolated, 2, NULL, 0);
        CHECK_SOLVE(&graph, EN_ADPT_FLOATING, &result);
        NEAR(result.terminals[0].voltage_v, 0.0, 1e-12);
        CHECK(!result.terminals[1].on_net && !result.terminals[2].on_net);
        CHECK(isnan(result.terminals[1].voltage_v));
        CHECK(isnan(result.terminals[2].voltage_v));
        NEAR(result.primitives[0].current_a, 0.0, 1e-15);
    }
}

int main(void)
{
    unfinished_wiring_remains_a_physical_state();
    abi_and_empty_graph();
    id_grammar_and_duplicates();
    endpoint_resolution_failures();
    divider_units_and_mapping();
    potentiometer_positions_and_merges();
    source_series_resistance_forms();
    switch_states();
    mcu_open_drain_pull_release_and_contention();
    floating_and_no_ground();
    unsupported_and_parameter_failures();
    same_net_shorts_and_source_conflicts();
    limits();
    determinism_hundred_repeats();
    printf("14 adapter scenarios passed (%u checks); no native GPIO/ADC claim\n",
           assertions);
    return 0;
}
