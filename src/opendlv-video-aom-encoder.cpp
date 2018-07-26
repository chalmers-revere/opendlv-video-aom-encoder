/*
 * Copyright (C) 2018  Christian Berger
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "cluon-complete.hpp"
#include "opendlv-standard-message-set.hpp"

#include <aom/aom_encoder.h>
#include <aom/aomcx.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

int32_t main(int32_t argc, char **argv) {
    int32_t retCode{1};
    auto commandlineArguments = cluon::getCommandlineArguments(argc, argv);
    if ( (0 == commandlineArguments.count("cid")) ||
         (0 == commandlineArguments.count("name")) ||
         (0 == commandlineArguments.count("width")) ||
         (0 == commandlineArguments.count("height")) ) {
        std::cerr << argv[0] << " attaches to an I420-formatted image residing in a shared memory area to convert it into a corresponding AV1 frame for publishing to a running OD4 session." << std::endl;
        std::cerr << "Usage:   " << argv[0] << " --cid=<OpenDaVINCI session> --name=<name of shared memory area> --width=<width> --height=<height> [--gop=<GOP>] [--bitrate=<bitrate>] [--verbose] [--id=<identifier in case of multiple instances]" << std::endl;
        std::cerr << "         --cid:     CID of the OD4Session to send AV1 frames" << std::endl;
        std::cerr << "         --id:      when using several instances, this identifier is used as senderStamp" << std::endl;
        std::cerr << "         --name:    name of the shared memory area to attach" << std::endl;
        std::cerr << "         --width:   width of the frame" << std::endl;
        std::cerr << "         --height:  height of the frame" << std::endl;
        std::cerr << "         --gop:     optional: length of group of pictures (default = 10)" << std::endl;
        std::cerr << "         --bitrate: optional: desired bitrate (default: 800,000, min: 50,000 max: 5,000,000)" << std::endl;
        std::cerr << "         --verbose: print encoding information" << std::endl;
        std::cerr << "Example: " << argv[0] << " --cid=111 --name=data --width=640 --height=480 --verbose" << std::endl;
    }
    else {
        const std::string NAME{commandlineArguments["name"]};
        const uint32_t WIDTH{static_cast<uint32_t>(std::stoi(commandlineArguments["width"]))};
        const uint32_t HEIGHT{static_cast<uint32_t>(std::stoi(commandlineArguments["height"]))};
        const uint32_t GOP_DEFAULT{10};
        const uint32_t GOP{(commandlineArguments["gop"].size() != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["gop"])) : GOP_DEFAULT};
        const uint32_t BITRATE_MIN{50000};
        const uint32_t BITRATE_DEFAULT{800000};
        const uint32_t BITRATE_MAX{5000000};
        const uint32_t BITRATE{(commandlineArguments["bitrate"].size() != 0) ? std::min(std::max(static_cast<uint32_t>(std::stoi(commandlineArguments["bitrate"])), BITRATE_MIN), BITRATE_MAX) : BITRATE_DEFAULT};
        const bool VERBOSE{commandlineArguments.count("verbose") != 0};
        const uint32_t ID{(commandlineArguments["id"].size() != 0) ? static_cast<uint32_t>(std::stoi(commandlineArguments["id"])) : 0};

        std::unique_ptr<cluon::SharedMemory> sharedMemory(new cluon::SharedMemory{NAME});
        if (sharedMemory && sharedMemory->valid()) {
            std::clog << "[opendlv-video-aom-encoder]: Attached to '" << sharedMemory->name() << "' (" << sharedMemory->size() << " bytes)." << std::endl;

            aom_image_t yuvFrame;
            memset(&yuvFrame, 0, sizeof(yuvFrame));
            if (!aom_img_alloc(&yuvFrame, AOM_IMG_FMT_I420, WIDTH, HEIGHT, 1)) {
                std::cerr << "[opendlv-video-aom-encoder]: Failed to allocate image." << std::endl;
                return retCode;
            }

            aom_codec_enc_cfg_t parameters;
            memset(&parameters, 0, sizeof(parameters));
            aom_codec_err_t result = aom_codec_enc_config_default(&aom_codec_av1_cx_algo, &parameters, 0);
            if (result) {
                std::cerr << "[opendlv-video-aom-encoder]: Failed to get default configuration: " << aom_codec_err_to_string(result) << std::endl;
                return retCode;
            }

            parameters.rc_target_bitrate = BITRATE/1000;
            parameters.g_w = WIDTH;
            parameters.g_h = HEIGHT;

            parameters.g_threads = 4;
            parameters.g_lag_in_frames = 0; // A value > 0 allows the encoder to consume more frames before emitting compressed frames.
            parameters.rc_end_usage = AOM_CBR;
            parameters.rc_undershoot_pct = 95;
            parameters.rc_buf_sz = 6000;
            parameters.rc_buf_initial_sz = 4000;
            parameters.rc_buf_optimal_sz = 5000;
            parameters.rc_min_quantizer = 4;
            parameters.rc_max_quantizer = 56;
            parameters.kf_max_dist = 999999;

            aom_codec_ctx_t codec;
            memset(&codec, 0, sizeof(codec));
            result = aom_codec_enc_init(&codec, &aom_codec_av1_cx_algo, &parameters, 0);
            if (result) {
                std::cerr << "[opendlv-video-aom-encoder]: Failed to initialize encoder: " << aom_codec_err_to_string(result) << std::endl;
                return retCode;
            }
            else {
                std::clog << "[opendlv-video-aom-encoder]: Using " << aom_codec_iface_name(&aom_codec_av1_cx_algo) << std::endl;
            }
            aom_codec_control(&codec, AOME_SET_CPUUSED, 4);

            // Allocate image buffer to hold VP9 frame as output.
            std::vector<char> aomBuffer;
            aomBuffer.resize(WIDTH * HEIGHT, '0'); // In practice, this is smaller than WIDTH * HEIGHT

            uint32_t frameCounter{0};

            cluon::data::TimeStamp before, after, sampleTimeStamp;

            // Interface to a running OpenDaVINCI session (ignoring any incoming Envelopes).
            cluon::OD4Session od4{static_cast<uint16_t>(std::stoi(commandlineArguments["cid"]))};

            while ( (sharedMemory && sharedMemory->valid()) && od4.isRunning() ) {
                // Wait for incoming frame.
                sharedMemory->wait();

                sampleTimeStamp = cluon::time::now();

                sharedMemory->lock();
                {
                    // TODO: Avoid copying the data.
                    memcpy(yuvFrame.planes[AOM_PLANE_Y], sharedMemory->data(), (WIDTH * HEIGHT));
                    memcpy(yuvFrame.planes[AOM_PLANE_U], sharedMemory->data() + (WIDTH * HEIGHT), ((WIDTH * HEIGHT) >> 2));
                    memcpy(yuvFrame.planes[AOM_PLANE_V], sharedMemory->data() + (WIDTH * HEIGHT + ((WIDTH * HEIGHT) >> 2)), ((WIDTH * HEIGHT) >> 2));
                    yuvFrame.stride[AOM_PLANE_Y] = WIDTH;
                    yuvFrame.stride[AOM_PLANE_U] = WIDTH/2;
                    yuvFrame.stride[AOM_PLANE_V] = WIDTH/2;
                }
                sharedMemory->unlock();

                if (VERBOSE) {
                    before = cluon::time::now();
                }
                int flags{ (0 == (frameCounter%GOP)) ? AOM_EFLAG_FORCE_KF : 0 };
                result = aom_codec_encode(&codec, &yuvFrame, frameCounter, 1, flags);
                if (result) {
                    std::cerr << "[opendlv-video-aom-encoder]: Failed to encode frame: " << aom_codec_err_to_string(result) << std::endl;
                }
                if (VERBOSE) {
                    after = cluon::time::now();
                }

                if (!result) {
                    aom_codec_iter_t it{nullptr};
                    const aom_codec_cx_pkt_t *packet{nullptr};

                    int totalSize{0};
                    while ((packet = aom_codec_get_cx_data(&codec, &it))) {
                        switch (packet->kind) {
                            case AOM_CODEC_CX_FRAME_PKT:
                                memcpy(&aomBuffer[totalSize], packet->data.frame.buf, packet->data.frame.sz);
                                totalSize += packet->data.frame.sz;
                            break;
                        default:
                            break;
                        }
                    }

                    if (0 < totalSize) {
                        opendlv::proxy::ImageReading ir;
                        ir.format("AV01").width(WIDTH).height(HEIGHT).data(std::string(&aomBuffer[0], totalSize));
                        od4.send(ir, sampleTimeStamp, ID);

                        if (VERBOSE) {
                            std::clog << "[opendlv-video-aom-encoder]: Frame size = " << totalSize << " bytes; encoding took " << cluon::time::deltaInMicroseconds(after, before) << " microseconds." << std::endl;
                        }
                        frameCounter++;
                    }
                }
            }

            aom_codec_destroy(&codec);

            retCode = 0;
        }
        else {
            std::cerr << "[opendlv-video-aom-encoder]: Failed to attach to shared memory '" << NAME << "'." << std::endl;
        }
    }
    return retCode;
}
