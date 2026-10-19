#include "audio_io.h"
#include "sdkconfig.h"

void app_main(void)
{
#if CONFIG_BARBIE_STAGE_2_TONE
    audio_io_init();
    stage2_tone_run();
#elif CONFIG_BARBIE_STAGE_3_MIC
    audio_io_init();
    stage3_mic_run();
#elif CONFIG_BARBIE_STAGE_4_HFP
    stage4_hfp_run();
#elif CONFIG_BARBIE_STAGE_5_CALL
    stage5_call_run();
#endif
}
