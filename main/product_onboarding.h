#pragma once

#include "bsp_button.h"

typedef void (*product_onboarding_complete_cb_t)(void);

void product_onboarding_enter(product_onboarding_complete_cb_t callback);
void product_onboarding_exit(void);
void product_onboarding_key(bsp_btn_t button, bsp_btn_ev_t event);
