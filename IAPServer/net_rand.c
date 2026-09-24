/*
 * net_rand.c
 *
 * See net_rand.h.
 */

#include "net_rand.h"
#include "rng.h"
#include <stdlib.h>

void net_rand_seed(void)
{
	uint32_t seed;

	if (HAL_RNG_GenerateRandomNumber(&hrng, &seed) == HAL_OK) {
		srand(seed);
	}
}

uint32_t net_rand_tcp_isn(void)
{
	uint32_t isn;

	if (HAL_RNG_GenerateRandomNumber(&hrng, &isn) != HAL_OK) {
		isn = (uint32_t)rand();
	}
	return isn;
}
