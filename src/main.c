/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include <switch.h>

#include "connection.h"
#include "configuration.h"
#include "config.h"
#include "platform.h"

#include "audio/audio.h"
#include "video/video.h"

#include "input/mapping.h"

#include <Limelight.h>

#include <client.h>
#include <discover.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/err.h>

#define MOONLIGHT_DATA_DIR "sdmc:/switch/moonlight-switch/"

static const SocketInitConfig customSocketInitConfig = {
    .bsdsockets_version = 1,

    .tcp_tx_buf_size        = 0x8000,
    .tcp_rx_buf_size        = 0x10000,
    .tcp_tx_buf_max_size    = 0x40000,
    .tcp_rx_buf_max_size    = 0x40000,

    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,

    .sb_efficiency = 4,

    .serialized_out_addrinfos_max_size  = 0x1000,
    .serialized_out_hostent_max_size    = 0x200,
    .bypass_nsd                         = false,
    .dns_timeout                        = 5,
};

static void applist(PSERVER_DATA server) {
  PAPP_LIST list = NULL;
  if (gs_applist(server, &list) != GS_OK) {
    fprintf(stderr, "Can't get app list\n");
    return;
  }

  for (int i = 1;list != NULL;i++) {
    printf("%d. %s\n", i, list->name);
    list = list->next;
  }
}

static int get_app_id(PSERVER_DATA server, const char *name) {
  PAPP_LIST list = NULL;
  if (gs_applist(server, &list) != GS_OK) {
    fprintf(stderr, "Can't get app list\n");
    return -1;
  }

  while (list != NULL) {
    if (strcmp(list->name, name) == 0)
      return list->id;

    list = list->next;
  }
  return -1;
}

static void stream(PSERVER_DATA server, PCONFIGURATION config, enum platform system) {
  int appId = get_app_id(server, config->app);
  if (appId<0) {
    fprintf(stderr, "Can't find app %s\n", config->app);
    exit(-1);
  }

  int gamepads = 0;
  int gamepad_mask;
  for (int i = 0; i < gamepads && i < 4; i++)
    gamepad_mask = (gamepad_mask << 1) + 1;

  int ret = gs_start_app(server, &config->stream, appId, config->sops, config->localaudio, gamepad_mask);
  if (ret < 0) {
    if (ret == GS_NOT_SUPPORTED_4K)
      fprintf(stderr, "Server doesn't support 4K\n");
    else if (ret == GS_NOT_SUPPORTED_MODE)
      fprintf(stderr, "Server doesn't support %dx%d (%d fps) or try --unsupported option\n", config->stream.width, config->stream.height, config->stream.fps);
    else if (ret == GS_ERROR)
      fprintf(stderr, "Gamestream error: %s\n", gs_error);
    else
      fprintf(stderr, "Errorcode starting app: %d\n", ret);
    exit(-1);
  }

  int drFlags = 0;
  if (config->fullscreen)
    drFlags |= DISPLAY_FULLSCREEN;

  if (config->debug_level > 0) {
    printf("Stream %d x %d, %d fps, %d kbps\n", config->stream.width, config->stream.height, config->stream.fps, config->stream.bitrate);
    connection_debug = true;
  }

  platform_start(system);
  LiStartConnection(&server->serverInfo, &config->stream, &connection_callbacks, platform_get_video(system), platform_get_audio(system, config->audio_device), NULL, drFlags, config->audio_device, 0);

  printf("Would loop here...");

  LiStopConnection();
  platform_stop(system);
}

static void help() {
  printf("Moonlight Embedded %d.%d.%d\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
  printf("Usage: moonlight [action] (options) [host]\n");
  printf("       moonlight [configfile]\n");
  printf("\n Actions\n\n");
  printf("\tpair\t\t\tPair device with computer\n");
  printf("\tunpair\t\t\tUnpair device with computer\n");
  printf("\tstream\t\t\tStream computer to device\n");
  printf("\tlist\t\t\tList available games and applications\n");
  printf("\tquit\t\t\tQuit the application or game being streamed\n");
  printf("\tmap\t\t\tCreate mapping for gamepad\n");
  printf("\thelp\t\t\tShow this help\n");
  printf("\n Global Options\n\n");
  printf("\t-config <config>\tLoad configuration file\n");
  printf("\t-save <config>\t\tSave configuration file\n");
  printf("\t-verbose\t\tEnable verbose output\n");
  printf("\t-debug\t\t\tEnable verbose and debug output\n");
  printf("\n Streaming options\n\n");
  printf("\t-720\t\t\tUse 1280x720 resolution [default]\n");
  printf("\t-1080\t\t\tUse 1920x1080 resolution\n");
  printf("\t-4k\t\t\tUse 3840x2160 resolution\n");
  printf("\t-width <width>\t\tHorizontal resolution (default 1280)\n");
  printf("\t-height <height>\tVertical resolution (default 720)\n");
  printf("\t-fps <fps>\t\tSpecify the fps to use (default -1)\n");
  printf("\t-bitrate <bitrate>\tSpecify the bitrate in Kbps\n");
  printf("\t-packetsize <size>\tSpecify the maximum packetsize in bytes\n");
  printf("\t-codec <codec>\t\tSelect used codec: auto/h264/h265 (default auto)\n");
  printf("\t-remote\t\t\tEnable remote optimizations\n");
  printf("\t-app <app>\t\tName of app to stream\n");
  printf("\t-nosops\t\t\tDon't allow GFE to modify game settings\n");
  printf("\t-localaudio\t\tPlay audio locally\n");
  printf("\t-surround\t\tStream 5.1 surround sound (requires GFE 2.7)\n");
  printf("\t-keydir <directory>\tLoad encryption keys from directory\n");
  printf("\t-mapping <file>\t\tUse <file> as gamepad mappings configuration file\n");
  printf("\t-platform <system>\tSpecify system used for audio, video and input: pi/imx/aml/x11/x11_vdpau/sdl/fake (default auto)\n");
  printf("\t-unsupported\t\tTry streaming if GFE version or options are unsupported\n");
  printf("\n WM options (SDL and X11 only)\n\n");
  printf("\t-windowed\t\tDisplay screen in a window\n");
  printf("\nUse Ctrl+Alt+Shift+Q or Play+Back+LeftShoulder+RightShoulder to exit streaming session\n\n");
  exit(0);
}

static int pair_check(PSERVER_DATA server) {
  if (!server->paired) {
    fprintf(stderr, "You must pair with the PC first\n");
    return 0;
  }

  return 1;
}

int main(int argc, char* argv[]) {
  gfxInitDefault();

  //Initialize console. Using NULL as the second argument tells the console library to use the internal console structure as current one.
  consoleInit(NULL);
  socketInitializeDefault();
  nxlinkStdio();

  // Initialize OpenSSL
  SSL_library_init();
  OpenSSL_add_all_algorithms();
  ERR_load_crypto_strings();

  // Seed the OpenSSL PRNG
  size_t seedlen = 2048;
  void *seedbuf = malloc(seedlen);
  csrngGetRandomBytes(seedbuf, seedlen);
  RAND_seed(seedbuf, seedlen);

  // Parse the global Moonlight settings
  CONFIGURATION config;
  config_parse(MOONLIGHT_DATA_DIR "moonlight.ini", &config);
  config.debug_level = 2;

  if (config.debug_level > 0)
    printf("Moonlight Embedded %d.%d.%d (%s)\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, COMPILE_OPTIONS);

  // Discover applicable streaming servers on the network
  if (config.address == NULL) {
    config.address = malloc(MAX_ADDRESS_SIZE);
    if (config.address == NULL) {
      perror("Not enough memory");
      return 0;
    }
    config.address[0] = 0;
    printf("Searching for server...\n");
    gs_discover_server(config.address);
    if (config.address[0] == 0) {
      fprintf(stderr, "Autodiscovery failed. Specify an IP address next time.\n");
      return 0;
    }
  }

  // Parse configuration specific to the host at `address`
  char host_config_file[128];
  sprintf(host_config_file, MOONLIGHT_DATA_DIR "hosts/%s.ini", config.address);

  if (access(host_config_file, R_OK) != -1)
    config_parse(host_config_file, &config);

  // Connect to the given host
  SERVER_DATA server;
  printf("Connect to %s...\n", config.address);

  int ret = gs_init(&server, config.address, MOONLIGHT_DATA_DIR "key", config.debug_level, config.unsupported);
  if (ret == GS_OUT_OF_MEMORY) {
    fprintf(stderr, "Not enough memory\n");
  } else if (ret == GS_ERROR) {
    fprintf(stderr, "Gamestream error: %s\n", gs_error);
  } else if (ret == GS_INVALID) {
    fprintf(stderr, "Invalid data received from server: %s\n", gs_error);
  } else if (ret == GS_UNSUPPORTED_VERSION) {
    fprintf(stderr, "Unsupported version: %s\n", gs_error);
  } else if (ret != GS_OK) {
    fprintf(stderr, "Can't connect to server %s, error: \n", config.address, gs_error);
  }

  if (config.debug_level > 0)
    printf("NVIDIA %s, GFE %s (%s, %s)\n", server.gpuType, server.serverInfo.serverInfoGfeVersion, server.gsVersion, server.serverInfo.serverInfoAppVersion);

  printf("\n");
  printf("A: list\n");
  printf("B: stream\n");
  printf("X: pair\n");
  printf("Y: unpair\n");
  printf("+: quit\n");
  printf("\n");

  while(appletMainLoop())
  {
      //Scan all the inputs. This should be done once for each frame
      hidScanInput();

      //hidKeysDown returns information about which buttons have been just pressed (and they weren't in the previous frame)
      u64 kDown = hidKeysDown(CONTROLLER_P1_AUTO);

      if (kDown & KEY_A) {
        if (pair_check(&server)) {
          applist(&server);
        }
      }
      else if (kDown & KEY_B) {
        if (pair_check(&server)) {
          enum platform system = platform_check(config.platform);
          if (config.debug_level > 0)
            printf("Beginning streaming on platform %s\n", platform_name(system));

          if (system == 0) {
            fprintf(stderr, "Platform '%s' not found\n", config.platform);
            break;
          }
          config.stream.supportsHevc = config.codec != CODEC_H264 && (config.codec == CODEC_HEVC || platform_supports_hevc(system));

          stream(&server, &config, system);
        }
      }
      else if (kDown & KEY_X) {
        char pin[5];
        sprintf(pin, "%d%d%d%d", (int)random() % 10, (int)random() % 10, (int)random() % 10, (int)random() % 10);
        printf("Please enter the following PIN on the target PC: %s\n", pin);
        if (gs_pair(&server, &pin[0]) != GS_OK) {
          fprintf(stderr, "Failed to pair to server: %s\n", gs_error);
        } else {
          printf("Succesfully paired\n");
        }
      }
      else if (kDown & KEY_Y) {
        if (gs_unpair(&server) != GS_OK) {
          fprintf(stderr, "Failed to unpair to server: %s\n", gs_error);
        } else {
          printf("Succesfully unpaired\n");
        }
      }
      else if (kDown & KEY_PLUS) {
        break;
      }

      gfxFlushBuffers();
      gfxSwapBuffers();
      gfxWaitForVsync();
  }

  gfxExit();
  return 0;
}
