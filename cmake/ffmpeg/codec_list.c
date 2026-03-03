static const AVCodec* const codec_list[] = {
#if CONFIG_MP3FLOAT_DECODER
    &ff_mp3float_decoder,
#endif
#if CONFIG_MP3_DECODER
    &ff_mp3_decoder,
#endif
#if CONFIG_WMAPRO_DECODER
    &ff_wmapro_decoder,
#endif
#if CONFIG_WMAV2_DECODER
    &ff_wmav2_decoder,
#endif
#if CONFIG_XMAFRAMES_DECODER
    &ff_xmaframes_decoder,
#endif
    NULL};
