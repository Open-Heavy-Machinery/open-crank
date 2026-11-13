/**
 * @file hydraulic_pto.h
 * @brief Simulation model for a hydraulic Power Take-Off (PTO) pump load connected to the engine.
 */

#ifndef OPEN_CRANK_HYDRAULIC_PTO_H
#define OPEN_CRANK_HYDRAULIC_PTO_H

/**
 * @brief Configuration parameters and runtime state for a hydraulic PTO pump.
 */
typedef struct {
    double displacement_cc_rev;  /**< Volumetric pump displacement in cc/rev (e.g., 140.0 cc/rev) */
    double mech_efficiency;      /**< Hydromechanical efficiency factor [0.0, 1.0] (e.g., 0.90) */
    double base_pressure_bar;    /**< Unloaded standby system pressure in bar (e.g., 20.0 bar) */
    double max_pressure_bar;     /**< Maximum relief/operating pressure in bar (e.g., 300.0 bar) */
    double current_pressure_bar; /**< Instantaneous hydraulic line pressure in bar */
} HydraulicPto;

/**
 * @brief Initializes the hydraulic PTO structure with default operating parameters and baseline state.
 * 
 * @param pto Pointer to the HydraulicPto instance to initialize.
 */
void hydraulic_pto_init(HydraulicPto *pto);

/**
 * @brief Advances the hydraulic PTO simulation by one time step and computes resistive load torque.
 * 
 * @param pto Pointer to the HydraulicPto instance.
 * @param valve_demand_0_to_1 Normalized proportional valve/joystick demand command [0.0, 1.0].
 * @param engine_rpm Current crankshaft speed driving the pump (RPM).
 * @param dt Simulation time step delta in seconds (s).
 * @return Resistive load torque applied to the engine crankshaft in Newton-meters (Nm).
 */
double hydraulic_pto_step(HydraulicPto *pto, double valve_demand_0_to_1, double engine_rpm, double dt);

#endif /* OPEN_CRANK_HYDRAULIC_PTO_H */
