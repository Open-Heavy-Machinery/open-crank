/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file dbc_vectors.c
 * @brief Frames from open-crank's own encoders, for tools/check_dbc.py.
 *
 * open-crank.dbc is the bus contract (docs/bus-interface.md) in machine-
 * readable form. To keep the two from drifting apart, this program encodes
 * frames with the code that really puts them on the bus, decodes them with
 * open-crank's own decoders, and prints, for each frame:
 *
 *     frame,<MESSAGE>,<29-bit id, hex>,<data, hex>,<Signal>=<value>,...
 *
 * where each value is what open-crank means by that field (a decoded
 * physical value, or the raw field for enumerations and bit fields). It also
 * prints the periods the bench and the controller really use:
 *
 *     cycle,<MESSAGE>,<ms>
 *
 * tools/check_dbc.py decodes the same frames with the DBC (cantools) and
 * requires every signal to match, and every period to be the DBC's
 * GenMsgCycleTime.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <opencrank/j1939.h>
#include <opencrank/j1939_diag.h>
#include <opencrank/j1939_diag_tx.h>
#include <opencrank/machine_ctrl.h>
#include <opencrank/oc_msgs.h>
#include <opencrank/runtime/bench.h>
#include <opencrank/runtime/loop.h>

static void frame_head(const char *name, const J1939Frame *f) {
    printf("frame,%s,%08lX,", name, (unsigned long)f->id);
    for (uint8_t i = 0U; i < f->dlc; i++) {
        printf("%02X", (unsigned)f->data[i]);
    }
}

static void sig(const char *name, double v) {
    printf(",%s=%.6f", name, v);
}

static void end(void) {
    printf("\n");
}

static void tsc1(J1939OverrideMode mode, float rpm, float torque, uint8_t counter, uint8_t prio) {
    const J1939Tsc1 m = {
        .override_mode = mode,
        .speed_conditions = 1U,
        .mode_priority = prio,
        .requested_speed_valid = true,
        .requested_speed_rpm = rpm,
        .requested_torque_valid = true,
        .requested_torque_pct = torque,
        .message_counter = counter,
    };
    J1939Frame f;
    j1939_encode_tsc1(&m, OC_ADDR_MACHINE_CTRL, J1939_ADDR_ENGINE_1, &f);
    J1939Tsc1 d;
    if (!j1939_decode_tsc1(&f, &d) || !d.checksum_valid) {
        fprintf(stderr, "dbc_vectors: TSC1 does not decode\n");
        return;
    }
    frame_head("TSC1", &f);
    sig("OverrideControlMode", (double)d.override_mode);
    sig("RequestedSpeedControlConditions", (double)d.speed_conditions);
    sig("OverrideControlModePriority", (double)d.mode_priority);
    sig("RequestedSpeed", (double)d.requested_speed_rpm);
    sig("RequestedTorque", (double)d.requested_torque_pct);
    sig("MessageCounter", (double)d.message_counter);
    sig("MessageChecksum", (double)j1939_tsc1_checksum(&f, d.message_counter));
    end();
}

static void eec1(uint8_t mode, float demand, float actual, float rpm, uint8_t ctrl) {
    const J1939Eec1 m = { mode, true, demand, true, actual, true, rpm, ctrl };
    J1939Frame f;
    j1939_encode_eec1(&m, J1939_ADDR_ENGINE_1, &f);
    J1939Eec1 d;
    (void)j1939_decode_eec1(&f, &d);
    frame_head("EEC1", &f);
    sig("EngineTorqueMode", (double)d.torque_mode);
    sig("DriversDemandEngineTorque", (double)d.driver_demand_torque_pct);
    sig("ActualEngineTorque", (double)d.actual_torque_pct);
    sig("EngineSpeed", (double)d.engine_speed_rpm);
    sig("SourceAddressOfControllingDevice", (double)d.controlling_address);
    end();
}

static void et1(float c) {
    const J1939Et1 m = { true, c };
    J1939Frame f;
    j1939_encode_et1(&m, J1939_ADDR_ENGINE_1, &f);
    J1939Et1 d;
    (void)j1939_decode_et1(&f, &d);
    frame_head("ET1", &f);
    sig("EngineCoolantTemperature", (double)d.coolant_temp_c);
    end();
}

static void eflp1(float kpa) {
    const J1939Eflp1 m = { true, kpa };
    J1939Frame f;
    j1939_encode_eflp1(&m, J1939_ADDR_ENGINE_1, &f);
    J1939Eflp1 d;
    (void)j1939_decode_eflp1(&f, &d);
    frame_head("EFLP1", &f);
    sig("EngineOilPressure", (double)d.oil_pressure_kpa);
    end();
}

static void operator_frame(float rpm, float lever, bool reset) {
    const OcOperator m = { true, rpm, true, lever, reset };
    J1939Frame f;
    oc_encode_operator(&m, OC_ADDR_BENCH, &f);
    OcOperator d;
    (void)oc_decode_operator(&f, &d);
    frame_head("OC_OPERATOR", &f);
    sig("TargetEngineSpeed", (double)d.target_speed_rpm);
    sig("HoistLever", (double)d.hoist_demand_pct);
    sig("FaultResetButton", d.fault_reset_pressed ? 1.0 : 0.0);
    end();
}

static void pump(float pct) {
    const OcPumpCmd m = { true, pct };
    J1939Frame f;
    oc_encode_pump_cmd(&m, OC_ADDR_MACHINE_CTRL, &f);
    OcPumpCmd d;
    (void)oc_decode_pump_cmd(&f, &d);
    frame_head("OC_PUMP_CMD", &f);
    sig("PumpDisplacementCommand", (double)d.displacement_pct);
    end();
}

/* A DM1 the way the engine sends it: one frame, or a BAM with two codes. */
static void dm1(uint8_t red, uint8_t amber, uint8_t n_dtc) {
    J1939Dm1 m;
    memset(&m, 0, sizeof(m));
    m.red_stop = red;
    m.amber_warning = amber;
    m.n_dtc = n_dtc;
    m.dtc[0] = (J1939Dtc){ 100U, 1U, 1U };       /* SPN 100, FMI 1 */
    m.dtc[1] = (J1939Dtc){ 524287U, 16U, 126U }; /* largest SPN, FMI 16 */
    uint8_t bytes[J1939_DM1_MAX_LEN];
    const size_t len = j1939_encode_dm1(&m, bytes, sizeof(bytes));
    J1939Dm1 d;
    (void)j1939_decode_dm1(bytes, len, &d);
    if (len <= 8U) {
        J1939Frame f;
        f.id = j1939_make_id(J1939_PRIORITY_INFO, J1939_PGN_DM1, J1939_ADDR_GLOBAL, J1939_ADDR_ENGINE_1);
        f.dlc = 8U;
        memcpy(f.data, bytes, 8U);
        frame_head("DM1", &f);
        sig("ProtectLampStatus", (double)d.protect);
        sig("AmberWarningLampStatus", (double)d.amber_warning);
        sig("RedStopLampStatus", (double)d.red_stop);
        sig("MalfunctionIndicatorLampStatus", (double)d.mil);
        const uint32_t spn = (d.n_dtc > 0U) ? d.dtc[0].spn : 0U;
        sig("DTC1_SPN_Low", (double)(spn & 0xFFFFU));
        sig("DTC1_SPN_High", (double)((spn >> 16U) & 0x07U));
        sig("DTC1_FMI", (double)((d.n_dtc > 0U) ? d.dtc[0].fmi : 0U));
        sig("DTC1_OccurrenceCount", (double)((d.n_dtc > 0U) ? d.dtc[0].oc : 0U));
        sig("DTC1_ConversionMethod", 0.0);
        end();
        return;
    }
    J1939Frame bam[J1939_TP_MAX_PACKETS + 1U];
    const size_t n = j1939_bam_encode(J1939_PGN_DM1, bytes, len, J1939_ADDR_ENGINE_1, bam, J1939_TP_MAX_PACKETS + 1U);
    frame_head("TP_CM_BAM", &bam[0]);
    sig("ControlByte", (double)J1939_TP_CM_BAM);
    sig("TotalMessageSize", (double)len);
    sig("TotalNumberOfPackets", (double)(n - 1U));
    sig("Reserved", 255.0);
    sig("PGNOfPacketedMessage", (double)J1939_PGN_DM1);
    end();
    for (size_t k = 1U; k < n; k++) {
        frame_head("TP_DT", &bam[k]);
        sig("SequenceNumber", (double)k);
        uint64_t packed = 0U; /* bytes 1-7, little endian; 56 bits do not fit a double exactly */
        for (int i = 7; i >= 1; i--) {
            packed = (packed << 8U) | bam[k].data[i];
        }
        printf(",PacketData=%llu", (unsigned long long)packed);
        end();
    }
}

static void bench_only(const char *name, uint32_t id, uint32_t seq) {
    J1939Frame f;
    f.id = id;
    f.dlc = 8U;
    memset(f.data, 0xFF, sizeof(f.data));
    if (seq != 0xFFFFFFFFUL) {
        for (unsigned i = 0U; i < 4U; i++) {
            f.data[i] = (uint8_t)(seq >> (8U * i));
        }
    }
    frame_head(name, &f);
    if (seq != 0xFFFFFFFFUL) {
        sig("Sequence", (double)seq);
    }
    end();
}

int main(void) {
    tsc1(J1939_OVERRIDE_SPEED, 1500.0f, 0.0f, 0U, 3U);
    tsc1(J1939_OVERRIDE_DISABLED, 850.125f, -125.0f, 7U, 0U);
    tsc1(J1939_OVERRIDE_SPEED, 2100.0f, 125.0f, 15U, 2U);
    eec1(J1939_TORQUE_MODE_LOW_IDLE, 37.0f, -12.0f, 1499.875f, OC_ADDR_MACHINE_CTRL);
    eec1(J1939_TORQUE_MODE_NA, 0.0f, 100.0f, 0.0f, 0xFFU);
    et1(-40.0f);
    et1(88.0f);
    eflp1(0.0f);
    eflp1(412.0f);
    operator_frame(1500.0f, 85.2f, false);
    operator_frame(0.0f, 0.0f, true);
    pump(85.2f);
    pump(100.0f);
    dm1(J1939_LAMP_OFF, J1939_LAMP_OFF, 0U);
    dm1(J1939_LAMP_ON, J1939_LAMP_OFF, 1U);
    dm1(J1939_LAMP_ON, J1939_LAMP_ON, 2U); /* a BAM: TP.CM and TP.DT */
    bench_only("BENCH_ECHO_REQUEST", 0x18FF2EFCUL, 123456789UL);
    bench_only("BENCH_ECHO", 0x18FF2F27UL, 42UL);
    bench_only("BENCH_HANG_TEST", 0x18FF2DFCUL, 0xFFFFFFFFUL);

    /* The periods the bench and the controller really use. */
    const OcBenchConfig *c = &OC_BENCH_D936LA6;
    static OcLoop loop; /* large: keep it off the stack */
    oc_loop_init(&loop, c, &MC_DEFAULT_PARAMS, NULL);
    printf("cycle,EEC1,%u\n", (unsigned)c->eec1_period_ms);
    printf("cycle,ET1,%u\n", (unsigned)c->et1_period_ms);
    printf("cycle,EFLP1,%u\n", (unsigned)c->eflp1_period_ms);
    printf("cycle,OC_OPERATOR,%u\n", (unsigned)c->operator_period_ms);
    printf("cycle,DM1,%u\n", (unsigned)c->dm1_period_ms);
    printf("cycle,TSC1,%u\n", (unsigned)loop.mc_period_ms);        /* one TSC1 per controller step */
    printf("cycle,OC_PUMP_CMD,%u\n", (unsigned)loop.mc_period_ms); /* one OC_PUMP_CMD per controller step */
    return 0;
}
