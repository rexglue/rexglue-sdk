static const AVCodecParser* const parser_list[] = {
#if CONFIG_MPEGAUDIO_PARSER
    &ff_mpegaudio_parser,
#endif
    NULL};
