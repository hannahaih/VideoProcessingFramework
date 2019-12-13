/*
 * Copyright 2019 NVIDIA Corporation
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "FFmpegDemuxer.h"
#include "NvCodecUtils.h"
#include <limits>

using namespace std;

simplelogger::Logger *logger;

class DataProvider {
public:
  virtual ~DataProvider() = default;
  virtual int GetData(uint8_t *pBuf, int nBuf) = 0;
};

FFmpegDemuxer::FFmpegDemuxer(const char *szFilePath)
    : FFmpegDemuxer(CreateFormatContext(szFilePath)) {}

FFmpegDemuxer::FFmpegDemuxer(DataProvider *pDataProvider)
    : FFmpegDemuxer(CreateFormatContext(pDataProvider)) {
  avioc = fmtc->pb;
}

uint32_t FFmpegDemuxer::GetWidth() const { return width; }

uint32_t FFmpegDemuxer::GetHeight() const { return height; }

uint32_t FFmpegDemuxer::GetFramerate() const { return framerate; }

uint32_t FFmpegDemuxer::GetVideoStreamIndex() const { return videoStream; }

bool FFmpegDemuxer::Demux(uint8_t *&pVideo, size_t &rVideoBytes) {
  if (!fmtc) {
    return false;
  }

  if (pkt.data) {
    av_packet_unref(&pkt);
  }

  if (!videoBytes.empty()) {
    videoBytes.clear();
  }

  auto appendBytes = [](vector<uint8_t> &elementaryBytes, AVPacket &avPacket,
                        AVPacket &avPacketBsf, AVBSFContext *pAvbsfContext,
                        int streamId, bool isFilteringNeeded) {
    if (avPacket.stream_index != streamId) {
      return;
    }

    if (isFilteringNeeded) {
      if (avPacketBsf.data) {
        av_packet_unref(&avPacketBsf);
      }

      ck(av_bsf_send_packet(pAvbsfContext, &avPacket));
      ck(av_bsf_receive_packet(pAvbsfContext, &avPacketBsf));

      elementaryBytes.insert(elementaryBytes.end(), avPacketBsf.data,
                             avPacketBsf.data + avPacketBsf.size);
    } else {
      elementaryBytes.insert(elementaryBytes.end(), avPacket.data,
                             avPacket.data + avPacket.size);
    }
  };

  int ret = 0;
  bool isDone = false, gotVideo = false;

  while (!isDone) {
    ret = av_read_frame(fmtc, &pkt);
    gotVideo = (pkt.stream_index == videoStream);
    isDone = (ret < 0) || gotVideo;

    /* Unref non-desired packets as we don't support them yet;
     */
    if (pkt.stream_index != videoStream) {
      av_packet_unref(&pkt);
      continue;
    }
  }

  if (ret < 0) {
    return false;
  }

  appendBytes(videoBytes, pkt, pktFiltered, bsfc, videoStream,
              is_mp4H264 || is_mp4HEVC);

  pVideo = videoBytes.data();
  rVideoBytes = videoBytes.size();

  // Update last packet data;
  lastPacketData.dts = pktFiltered.dts;
  lastPacketData.duration = pktFiltered.duration;
  lastPacketData.pos = pktFiltered.pos;
  lastPacketData.pts = pktFiltered.pts;

  return true;
}

void FFmpegDemuxer::GetLastPacketData(PacketData &pktData) {
  pktData = lastPacketData;
}

int FFmpegDemuxer::ReadPacket(void *opaque, uint8_t *pBuf, int nBuf) {
  return ((DataProvider *)opaque)->GetData(pBuf, nBuf);
}

AVCodecID FFmpegDemuxer::GetVideoCodec() const { return eVideoCodec; }

FFmpegDemuxer::~FFmpegDemuxer() {
  if (pkt.data) {
    av_packet_unref(&pkt);
  }
  if (pktFiltered.data) {
    av_packet_unref(&pktFiltered);
  }

  if (bsfc) {
    av_bsf_free(&bsfc);
  }

  avformat_close_input(&fmtc);

  if (avioc) {
    av_freep(&avioc->buffer);
    av_freep(&avioc);
  }
}

AVFormatContext *
FFmpegDemuxer::CreateFormatContext(DataProvider *pDataProvider) {
  AVFormatContext *ctx = nullptr;
  if (!(ctx = avformat_alloc_context())) {
    LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__;
    return nullptr;
  }

  uint8_t *avioc_buffer = nullptr;
  int avioc_buffer_size = 8 * 1024 * 1024;
  avioc_buffer = (uint8_t *)av_malloc(avioc_buffer_size);
  if (!avioc_buffer) {
    LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__;
    return nullptr;
  }
  avioc = avio_alloc_context(avioc_buffer, avioc_buffer_size, 0, pDataProvider,
                             &ReadPacket, nullptr, nullptr);

  if (!avioc) {
    LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__;
    return nullptr;
  }
  ctx->pb = avioc;

  ck(avformat_open_input(&ctx, nullptr, nullptr, nullptr));
  return ctx;
}

AVFormatContext *FFmpegDemuxer::CreateFormatContext(const char *szFilePath) {
  avformat_network_init();
  std::cout << szFilePath << std::endl;

  AVFormatContext *ctx = nullptr;
  ck(avformat_open_input(&ctx, szFilePath, nullptr, nullptr));
  return ctx;
}

FFmpegDemuxer::FFmpegDemuxer(AVFormatContext *fmtcx) : fmtc(fmtcx) {
  pkt = {};
  pktFiltered = {};

  if (!fmtc) {
    LOG(ERROR) << "No AVFormatContext provided.";
    return;
  }

  LOG(INFO) << "Media format: " << fmtc->iformat->long_name << " ("
            << fmtc->iformat->name << ")";

  ck(avformat_find_stream_info(fmtc, nullptr));

  videoStream =
      av_find_best_stream(fmtc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (videoStream < 0) {
    LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " "
               << "Could not find video stream in input file";
    return;
  }

  eVideoCodec = fmtc->streams[videoStream]->codecpar->codec_id;
  width = fmtc->streams[videoStream]->codecpar->width;
  height = fmtc->streams[videoStream]->codecpar->height;
  framerate = fmtc->streams[videoStream]->r_frame_rate.num;
  eChromaFormat = (AVPixelFormat)fmtc->streams[videoStream]->codecpar->format;

  is_mp4H264 = eVideoCodec == AV_CODEC_ID_H264 &&
               (!strcmp(fmtc->iformat->long_name, "QuickTime / MOV") ||
                !strcmp(fmtc->iformat->long_name, "FLV (Flash Video)") ||
                !strcmp(fmtc->iformat->long_name, "Matroska / WebM"));
  is_mp4HEVC = eVideoCodec == AV_CODEC_ID_HEVC &&
               (!strcmp(fmtc->iformat->long_name, "QuickTime / MOV") ||
                !strcmp(fmtc->iformat->long_name, "FLV (Flash Video)") ||
                !strcmp(fmtc->iformat->long_name, "Matroska / WebM"));
  av_init_packet(&pkt);
  pkt.data = nullptr;
  pkt.size = 0;
  av_init_packet(&pktFiltered);
  pktFiltered.data = nullptr;
  pktFiltered.size = 0;

  if (is_mp4H264) {
    const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
    if (!bsf) {
      LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " "
                 << "av_bsf_get_by_name() failed";
      return;
    }
    ck(av_bsf_alloc(bsf, &bsfc));
    avcodec_parameters_copy(bsfc->par_in, fmtc->streams[videoStream]->codecpar);
    ck(av_bsf_init(bsfc));
  }
  if (is_mp4HEVC) {
    const AVBitStreamFilter *bsf = av_bsf_get_by_name("hevc_mp4toannexb");
    if (!bsf) {
      LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " "
                 << "av_bsf_get_by_name() failed";
      return;
    }
    ck(av_bsf_alloc(bsf, &bsfc));
    avcodec_parameters_copy(bsfc->par_in, fmtc->streams[videoStream]->codecpar);
    ck(av_bsf_init(bsfc));
  }
}