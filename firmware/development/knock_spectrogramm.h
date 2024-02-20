/**
 * @file	knock_spectrogramm.h
 *
 * @date Feb 20, 2023
 * @author Alexey Ershov, (c) 2012-2023
 */

#pragma once

#include "global.h"

void knockSpectorgrammAddLine(float* data, size_t size);
void initKnockSpectrogramm(void);
void publishKnockSpectrogrammIfFull();
