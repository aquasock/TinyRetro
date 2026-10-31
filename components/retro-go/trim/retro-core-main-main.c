#include "shared.h"


void app_main(void)
{
    rg_app_t *app = rg_system_init(AUDIO_SAMPLE_RATE, NULL, NULL);

    RG_LOGI("configNs=%s", app->configNs);

    /* TinyRetro: only Game Boy / Game Boy Color and NES are built. The other
     * cores are removed from COMPONENTS in ../CMakeLists.txt, so their
     * front ends no longer exist; restore these branches together with the
     * component list. */
    if (strcmp(app->configNs, "gbc") == 0 || strcmp(app->configNs, "gb") == 0)
        gbc_main();
    else if (strcmp(app->configNs, "nes") == 0)
        nes_main();
    else
        launcher_main();

    RG_PANIC("Never reached");
}
