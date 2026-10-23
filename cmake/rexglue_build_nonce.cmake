# rexglue_get_build_nonce(<out>) returns the nonce of this build tree. The first call in a tree
# generates it and stores it in the cache, so a reconfigure keeps it and a new tree gets its own.
function (rexglue_get_build_nonce out)
  if (NOT _REXGLUE_BUILD_NONCE)
    string(
      RANDOM
      LENGTH 12
      ALPHABET "0123456789abcdef" nonce
    )
    set(_REXGLUE_BUILD_NONCE
        "${nonce}"
        CACHE INTERNAL "Identity nonce of this build tree"
    )
  endif ()
  set(${out}
      "${_REXGLUE_BUILD_NONCE}"
      PARENT_SCOPE
  )
endfunction ()
