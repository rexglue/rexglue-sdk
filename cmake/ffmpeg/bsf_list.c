static const AVBitStreamFilter* const bitstream_filters[] = {
#if CONFIG_NULL_BSF
    &ff_null_bsf,
#endif
    NULL};
