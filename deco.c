/* calculate deco values
 * based on Bühlmann ZHL-16b
 * based on an implemention by heinrichs weikamp for the DR5
 * the original file was given to Subsurface under the GPLv2
 * by Matthias Heinrichs
 *
 * The implementation below is a fairly complete rewrite since then
 * (C) Robert C. Helling 2013 and released under the GPLv2
 *
 * add_segment()	- add <seconds> at the given pressure, breathing gasmix
 * deco_allowed_depth() - ceiling based on lead tissue, surface pressure, 3m increments or smooth
 * set_gf()		- set Buehlmann gradient factors
 * clear_deco()
 * cache_deco_state()
 * restore_deco_state()
 * dump_tissues()
 */
#include <math.h>
#include <string.h>
#include "dive.h"

//! Option structure for Buehlmann decompression.
struct buehlmann_config {
  double  satmult;		//! safety at inert gas accumulation as percentage of effect (more than 100).
  double  desatmult;		//! safety at inert gas depletion as percentage of effect (less than 100).
  int  last_deco_stop_in_mtr;	//! depth of last_deco_stop.
  double  gf_high;		//! gradient factor high (at surface).
  double  gf_low;		//! gradient factor low (at bottom/start of deco calculation).
  double  gf_low_position_min;	//! gf_low_position below surface_min_shallow.
};
struct buehlmann_config buehlmann_config = { 1.0, 1.01, 0, 0.75, 0.35, 2.0 };

const double buehlmann_N2_a[] = {1.1696, 1.0, 0.8618, 0.7562,
				 0.62, 0.5043, 0.441, 0.4,
				 0.375, 0.35, 0.3295, 0.3065,
				 0.2835, 0.261, 0.248, 0.2327};

const double buehlmann_N2_b[] = {0.5578, 0.6514, 0.7222, 0.7825,
				 0.8126, 0.8434, 0.8693, 0.8910,
				 0.9092, 0.9222, 0.9319, 0.9403,
				 0.9477, 0.9544, 0.9602, 0.9653};

const double buehlmann_N2_t_halflife[] = {5.0, 8.0, 12.5, 18.5,
					 27.0, 38.3, 54.3, 77.0,
					 109.0, 146.0, 187.0, 239.0,
					 305.0, 390.0, 498.0, 635.0};

const double buehlmann_N2_factor_expositon_one_second[] = {
	2.30782347297664E-003, 1.44301447809736E-003, 9.23769302935806E-004, 6.24261986779007E-004,
	4.27777107246730E-004, 3.01585140931371E-004, 2.12729727268379E-004, 1.50020603047807E-004,
	1.05980191127841E-004, 7.91232600646508E-005, 6.17759153688224E-005, 4.83354552742732E-005,
	3.78761777920511E-005, 2.96212356654113E-005, 2.31974277413727E-005, 1.81926738960225E-005};

const double buehlmann_He_a[] = { 1.6189, 1.383 , 1.1919, 1.0458,
				  0.922 , 0.8205, 0.7305, 0.6502,
				  0.595 , 0.5545, 0.5333, 0.5189,
				  0.5181, 0.5176, 0.5172, 0.5119};

const double buehlmann_He_b[] = {0.4770, 0.5747, 0.6527, 0.7223,
				 0.7582, 0.7957, 0.8279, 0.8553,
				 0.8757, 0.8903, 0.8997, 0.9073,
				 0.9122, 0.9171, 0.9217, 0.9267};

const double buehlmann_He_t_halflife[] = {1.88, 3.02, 4.72, 6.99,
					  10.21, 14.48, 20.53, 29.11,
					  41.20, 55.19, 70.69, 90.34,
					  115.29, 147.42, 188.24, 240.03};

const double buehlmann_He_factor_expositon_one_second[] = {
	6.12608039419837E-003, 3.81800836683133E-003, 2.44456078654209E-003, 1.65134647076792E-003,
	1.13084424730725E-003, 7.97503165599123E-004, 5.62552521860549E-004, 3.96776399429366E-004,
	2.80360036664540E-004, 2.09299583354805E-004, 1.63410794820518E-004, 1.27869320250551E-004,
	1.00198406028040E-004, 7.83611475491108E-005, 6.13689891868496E-005, 4.81280465299827E-005};

#define WV_PRESSURE 0.0627 // water vapor pressure in bar
#define DECO_STOPS_MULTIPLIER_MM 3000.0

#define GF_LOW_AT_MAXDEPTH 0

double tissue_n2_sat[16];
double tissue_he_sat[16];
int ci_pointing_to_guiding_tissue;
double gf_low_pressure_this_dive;
#define TISSUE_ARRAY_SZ sizeof(tissue_n2_sat)

static double tissue_tolerance_calc(const struct dive *dive)
{
	int ci = -1;
	double tissue_inertgas_saturation, buehlmann_inertgas_a, buehlmann_inertgas_b;
	double ret_tolerance_limit_ambient_pressure = 0.0;
	double gf_high = buehlmann_config.gf_high;
	double gf_low = buehlmann_config.gf_low;
	double surface = get_surface_pressure_in_mbar(dive, TRUE) / 1000.0;
	double lowest_ceiling;

	for (ci = 0; ci < 16; ci++)
	{
		double tolerated;

		tissue_inertgas_saturation = tissue_n2_sat[ci] + tissue_he_sat[ci];
		buehlmann_inertgas_a = ((buehlmann_N2_a[ci] * tissue_n2_sat[ci]) + (buehlmann_He_a[ci] * tissue_he_sat[ci])) / tissue_inertgas_saturation;
		buehlmann_inertgas_b = ((buehlmann_N2_b[ci] * tissue_n2_sat[ci]) + (buehlmann_He_b[ci] * tissue_he_sat[ci])) / tissue_inertgas_saturation;

		/* tolerated = (tissue_inertgas_saturation - buehlmann_inertgas_a) * buehlmann_inertgas_b; */

#if !GF_LOW_AT_MAXDEPTH
		lowest_ceiling = (buehlmann_inertgas_b * tissue_inertgas_saturation - gf_low * buehlmann_inertgas_a * buehlmann_inertgas_b) /
			((1.0 - buehlmann_inertgas_b) * gf_low + buehlmann_inertgas_b);
		if (lowest_ceiling > gf_low_pressure_this_dive)
			gf_low_pressure_this_dive = lowest_ceiling;
#endif

		tolerated = (-buehlmann_inertgas_a * buehlmann_inertgas_b * (gf_high * gf_low_pressure_this_dive - gf_low * surface) -
				(1.0 - buehlmann_inertgas_b) * (gf_high - gf_low) * gf_low_pressure_this_dive * surface +
				buehlmann_inertgas_b * (gf_low_pressure_this_dive - surface) * tissue_inertgas_saturation) /
			    (-buehlmann_inertgas_a * buehlmann_inertgas_b * (gf_high - gf_low) +
				(1.0 - buehlmann_inertgas_b)*(gf_low * gf_low_pressure_this_dive - gf_high * surface) +
				buehlmann_inertgas_b * (gf_low_pressure_this_dive - surface));

		if (tolerated > ret_tolerance_limit_ambient_pressure)
		{
			ci_pointing_to_guiding_tissue = ci;
			ret_tolerance_limit_ambient_pressure = tolerated;
		}
	}
	return ret_tolerance_limit_ambient_pressure;
}

/* add period_in_seconds at the given pressure and gas to the deco calculation */
double add_segment(double pressure, const struct gasmix *gasmix, int period_in_seconds, int ccpo2, const struct dive *dive)
{
	int ci;
	int fo2 = gasmix->o2.permille ? gasmix->o2.permille : O2_IN_AIR;
	double ppn2 = (pressure - WV_PRESSURE) * (1000 - fo2 - gasmix->he.permille) / 1000.0;
	double pphe = (pressure - WV_PRESSURE) * gasmix->he.permille / 1000.0;

#if GF_LOW_AT_MAXDEPTH
	if (pressure > gf_low_pressure_this_dive)
	        gf_low_pressure_this_dive = pressure;
#endif

	if (ccpo2) { /* CC */
		double rel_o2_amb, f_dilutent;
		rel_o2_amb = ccpo2 / pressure / 1000;
		f_dilutent = (1 - rel_o2_amb) / (1 - fo2 / 1000.0);
		if (f_dilutent < 0) { /* setpoint is higher than ambient pressure -> pure O2 */
			ppn2 = 0.0;
			pphe = 0.0;
		} else if (f_dilutent < 1.0) {
			ppn2 *= f_dilutent;
			pphe *= f_dilutent;
		}
	}
	if (period_in_seconds == 1) { /* one second interval during dive */
		for (ci = 0; ci < 16; ci++) {
			if (ppn2 - tissue_n2_sat[ci] > 0)
				tissue_n2_sat[ci] += buehlmann_config.satmult * (ppn2 - tissue_n2_sat[ci]) *
								buehlmann_N2_factor_expositon_one_second[ci];
			else
				tissue_n2_sat[ci] += buehlmann_config.desatmult * (ppn2 - tissue_n2_sat[ci]) *
								buehlmann_N2_factor_expositon_one_second[ci];
			if (pphe - tissue_he_sat[ci] > 0)
				tissue_he_sat[ci] += buehlmann_config.satmult * (pphe - tissue_he_sat[ci]) *
								buehlmann_He_factor_expositon_one_second[ci];
			else
				tissue_he_sat[ci] += buehlmann_config.desatmult * (pphe - tissue_he_sat[ci]) *
								buehlmann_He_factor_expositon_one_second[ci];
		}
	} else { /* all other durations */
		for (ci = 0; ci < 16; ci++)
		{
			if (ppn2 - tissue_n2_sat[ci] > 0)
				tissue_n2_sat[ci] += buehlmann_config.satmult * (ppn2 - tissue_n2_sat[ci]) *
					(1 - pow(2.0,(- period_in_seconds / (buehlmann_N2_t_halflife[ci] * 60))));
			else
				tissue_n2_sat[ci] += buehlmann_config.desatmult * (ppn2 - tissue_n2_sat[ci]) *
					(1 - pow(2.0,(- period_in_seconds / (buehlmann_N2_t_halflife[ci] * 60))));
			if (pphe - tissue_he_sat[ci] > 0)
				tissue_he_sat[ci] += buehlmann_config.satmult * (pphe - tissue_he_sat[ci]) *
					(1 - pow(2.0,(- period_in_seconds / (buehlmann_He_t_halflife[ci] * 60))));
			else
				tissue_he_sat[ci] += buehlmann_config.desatmult * (pphe - tissue_he_sat[ci]) *
					(1 - pow(2.0,(- period_in_seconds / (buehlmann_He_t_halflife[ci] * 60))));
		}
	}
	return tissue_tolerance_calc(dive);
}

void dump_tissues()
{
	int ci;
	printf("N2 tissues:");
	for (ci = 0; ci < 16; ci++)
		printf(" %6.3e", tissue_n2_sat[ci]);
	printf("\nHe tissues:");
	for (ci = 0; ci < 16; ci++)
		printf(" %6.3e", tissue_he_sat[ci]);
	printf("\n");
}

void clear_deco(double surface_pressure)
{
	int ci;
	for (ci = 0; ci < 16; ci++) {
		tissue_n2_sat[ci] = (surface_pressure - WV_PRESSURE) * N2_IN_AIR / 1000;
		tissue_he_sat[ci] = 0.0;
	}
	gf_low_pressure_this_dive = surface_pressure + buehlmann_config.gf_low_position_min;
}

void cache_deco_state(double tissue_tolerance, char **cached_datap)
{
	char *data = *cached_datap;

	if (!data) {
		data = malloc(2 * TISSUE_ARRAY_SZ + 2 * sizeof(double) + sizeof(int));
		*cached_datap = data;
	}
	memcpy(data, tissue_n2_sat, TISSUE_ARRAY_SZ);
	data += TISSUE_ARRAY_SZ;
	memcpy(data, tissue_he_sat, TISSUE_ARRAY_SZ);
	data += TISSUE_ARRAY_SZ;
	memcpy(data, &gf_low_pressure_this_dive, sizeof(double));
	data += sizeof(double);
	memcpy(data, &tissue_tolerance, sizeof(double));
	data += sizeof(double);
	memcpy(data, &ci_pointing_to_guiding_tissue, sizeof(int));
}

double restore_deco_state(char *data)
{
	double tissue_tolerance;

	memcpy(tissue_n2_sat, data, TISSUE_ARRAY_SZ);
	data += TISSUE_ARRAY_SZ;
	memcpy(tissue_he_sat, data, TISSUE_ARRAY_SZ);
	data += TISSUE_ARRAY_SZ;
	memcpy(&gf_low_pressure_this_dive, data, sizeof(double));
	data += sizeof(double);
	memcpy(&tissue_tolerance, data, sizeof(double));
	data += sizeof(double);
	memcpy(&ci_pointing_to_guiding_tissue, data, sizeof(int));

	return tissue_tolerance;
}

unsigned int deco_allowed_depth(double tissues_tolerance, double surface_pressure, struct dive *dive, gboolean smooth)
{
	unsigned int depth;
	double pressure_delta;

	/* Avoid negative depths */
	pressure_delta = tissues_tolerance > surface_pressure ? tissues_tolerance - surface_pressure : 0.0;

	depth = rel_mbar_to_depth(pressure_delta * 1000, dive);

	if (!smooth)
		depth = ceil(depth / DECO_STOPS_MULTIPLIER_MM) * DECO_STOPS_MULTIPLIER_MM;

	if (depth > 0 && depth < buehlmann_config.last_deco_stop_in_mtr * 1000)
		depth = buehlmann_config.last_deco_stop_in_mtr * 1000;

	return depth;
}

void set_gf(double gflow, double gfhigh)
{
	if (gflow != -1.0)
		buehlmann_config.gf_low = gflow;
	if (gfhigh != -1.0)
		buehlmann_config.gf_high = gfhigh;
}
