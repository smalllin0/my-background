file(GLOB SOURCE_CODE
    "src/*.cc"
)


idf_component_register(
    SRCS
        ${SOURCE_CODE}
    INCLUDE_DIRS
        "include"
    REQUIRES
        "my-linklist"
    PRIV_REQUIRES
)
