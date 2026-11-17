/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file d936la6.h
 * @brief Plant `d936la6`: a representative 10.5 L inline-six heavy-duty diesel
 *        of the Liebherr D936L A6 class, and the calibration of its simulated
 *        engine ECU. Not calibrated against any real engine; see
 *        docs/parameters.md for the provenance of every number.
 */

#ifndef OPEN_CRANK_PLANTS_D936LA6_H
#define OPEN_CRANK_PLANTS_D936LA6_H

#include <opencrank/engine_ecu.h>
#include <opencrank/plant.h>

/** @brief Physical parameters of the d936la6 plant. */
extern const PlantParams PLANT_D936LA6;

/** @brief Calibration of the simulated engine ECU for the d936la6 plant. */
extern const EngineEcuParams ENGINE_ECU_D936LA6;

#endif /* OPEN_CRANK_PLANTS_D936LA6_H */
