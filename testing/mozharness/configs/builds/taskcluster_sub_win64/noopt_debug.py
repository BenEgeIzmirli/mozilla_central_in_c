config = {
    'stage_platform': 'win64-noopt-debug',
    'debug_build': True,
    'env': {
        'XPCOM_DEBUG_BREAK': 'stack-and-abort',
    },
    'mozconfig_variant': 'noopt-debug',
    'artifact_flag_build_variant_in_try': None,
}