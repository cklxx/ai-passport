// main/demo.h —— Feishu messenger page interface (the only product page).
#pragma once

#include "bsp_button.h"
#include <stdbool.h>

void demo_feishu_enter(void); void demo_feishu_exit(void);
void demo_feishu_key(bsp_btn_t btn, bsp_btn_ev_t ev);
bool demo_feishu_back(void);

void demo_voice_enter(void); void demo_voice_exit(void);
void demo_voice_key(bsp_btn_t btn, bsp_btn_ev_t ev);
