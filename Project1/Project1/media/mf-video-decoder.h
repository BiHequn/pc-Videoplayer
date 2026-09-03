#pragma once

#include"video-decoder.h"

#include<memory>

class MfVideoDecoder final :public IVideoDecoder
{
public:
	MfVideoDecoder();
	~MfVideoDecoder()override;

	bool initialize(const VideoDecoderConfig& config)override;
	VideoDecoderResult sendPacket(const EncodedVideoPacket& packet)override;
	VideoDecoderResult receiveFrame(DecodedVideoFrame* frame)override;

	VideoDecoderResult drain() override;

	void flush() override;
	void close() override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;

};