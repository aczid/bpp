#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "hexdump.h"


#include "structs.h"
#include "chksign.h"
#include "defec.h"
#include "serdec.h"
#include "hldemux.h"

#include "subtitle.h"
#include "blockdecode.h"
#include "bd_emu.h"
#include "bd_flatflash.h"
#include "hkpackets.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "powerdown.h"

#include "freertos/ringbuf.h"

#include "bpp_sniffer.h"


esp_err_t event_handler(void *ctx, system_event_t *event)
{
    return ESP_OK;
}



BlockDecodeHandle *otablockdecoder;

void flashDone(uint32_t changeId, void *arg) {
	esp_partition_t *otapart=(esp_partition_t*)arg;
	printf("Flash done!\n");
	esp_err_t r=esp_ota_set_boot_partition(otapart);
	if (r!=ESP_OK) {
		printf("Huh? esp_ota_set_boot_partition failed.\n");
	} else {
		blockdecodeShutDown(otablockdecoder);
		esp_restart();
	}
}

void doDeepSleep(int delayMs, void *arg) {
	delayMs-=3000; //to compensate for startup delay
	if (delayMs<5000) return; //not worth sleeping
	printf("Sleeping for %d ms...\n", delayMs);
	blockdecodeShutDown(otablockdecoder);
	esp_deep_sleep_enable_timer_wakeup(delayMs*1000);
	esp_deep_sleep_start();
}



void app_main(void)
{
    ESP_ERROR_CHECK( nvs_flash_init() );
    tcpip_adapter_init();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

	printf("************************************\n");
	printf("* "COMPILE_DATE"    \n");
	printf("************************************\n");

	//Initialize bpp components
	powerDownMgrInit(doDeepSleep, NULL);
	chksignInit(defecRecv);
	defecInit(serdecRecv, 1400);
	serdecInit(hldemuxRecv);
	
	//Grab last OTA firmware change ID so we don't redundantly update the OTA region
	nvs_handle nvsh=NULL;
	nvs_open("bpp", NVS_READWRITE, &nvsh);
	uint32_t chgid=0;
	nvs_get_u32(nvsh, "lastotaid", &chgid);
	nvs_close(nvsh);

	//Figure out which OTA partition we're *not* running from, pass that to the blockdev.
	const esp_partition_t *otapart=esp_ota_get_next_update_partition(NULL);

	if (otapart == NULL) {
		fprintf(stderr, "No OTA partition found, this is required for BPP\n");
		exit(-1);
	}
	
	BlockdefIfFlatFlashDesc bdesc={
		.major=otapart->type,
		.minor=otapart->subtype,
		.doneCb=flashDone,
		.doneCbArg=(void*)otapart,
		.minChangeId=chgid
	};
	int otasize=otapart->size-4096; //&blockdefIfFlatFlash uses the last block for metadata
	otablockdecoder=blockdecodeInit(1, otasize, &blockdefIfFlatFlash, &bdesc);
	printf("Initialized ota blockdev listener; maj=%x min=%x size=%d\n", bdesc.major, bdesc.minor, otasize);
	subtitleInit();
	hkpacketsInit();

	bppWifiSnifferStart();
}

