#include "battery.h"

void battery_init() {
    analogReadResolution(12);
    pinMode(BATTERY_ADC_PIN, INPUT);
}

uint32_t battery_read_mv() {
    // Average a few samples to smooth ADC noise.
    uint32_t sum = 0;
    const int N = 8;
    for (int i = 0; i < N; i++) sum += analogReadMilliVolts(BATTERY_ADC_PIN);
    uint32_t adc_mv = sum / N;
    return adc_mv * 2;  // 100k/100k divider halves the voltage
}

// Piecewise-linear discharge curve for a single-cell Li-ion/LiPo.
int battery_read_percent() {
    uint32_t mv = battery_read_mv();

    struct Point { uint32_t mv; int pct; };
    static const Point curve[] = {
        {4200, 100},
        {4000,  80},
        {3850,  60},
        {3700,  40},
        {3500,  20},
        {3300,  10},
        {3000,   0},
    };
    const int n = sizeof(curve) / sizeof(curve[0]);

    if (mv >= curve[0].mv) return 100;
    if (mv <= curve[n-1].mv) return 0;

    for (int i = 0; i < n - 1; i++) {
        if (mv <= curve[i].mv && mv >= curve[i+1].mv) {
            uint32_t span = curve[i].mv - curve[i+1].mv;
            uint32_t off  = mv - curve[i+1].mv;
            int pct_span  = curve[i].pct - curve[i+1].pct;
            return curve[i+1].pct + (int)((off * pct_span) / span);
        }
    }
    return 0;
}
