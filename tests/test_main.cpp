#include "minunit.h"

void sync_suite();
void hls_suite();
void player_suite();
void audio_suite();

int main() {
    MU_RUN_SUITE(sync_suite);
    MU_RUN_SUITE(hls_suite);
    MU_RUN_SUITE(player_suite);
    MU_RUN_SUITE(audio_suite);
    MU_REPORT();
    return 0;
}
