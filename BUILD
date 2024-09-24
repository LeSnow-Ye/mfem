load("@rules_cc//cc:defs.bzl", "cc_library")

### MFEM examples #############################################################
load("//:.bazel.bzl", "generate_examples")

generate_examples()

### MFEM library ##############################################################

cc_library(
    name = "mfem",
    deps = [
        "fem",
        "general",
        "linalg",
        "mesh",
    ],
)

### Sources ###################################################################

cc_library(
    name = "fem",
    srcs = glob([
        "fem/*.cpp",
        "fem/ceed/**/*.cpp",
        "fem/fe/*.cpp",
        "fem/integ/*.cpp",
        "fem/lor/*.cpp",
        # skip moonolith
        "fem/qinterp/*.cpp",
        "fem/tmop/*.cpp",
    ]),
    deps = [
        "config_hpp",
        "fem_hpp",
        "general_hpp",
        "linalg_hpp",
        "mesh_hpp",
    ],
)

cc_library(
    name = "general",
    srcs = glob(["general/*.cpp"]),
    deps = [
        "config_hpp",
        "general_hpp",
        "linalg_hpp",
    ],
)

cc_library(
    name = "linalg",
    srcs = glob(["linalg/**/*.cpp"]),
    deps = [
        "config_hpp",
        "fem_hpp",
        "general_hpp",
        "linalg_hpp",
        "mesh_hpp",
    ],
)

cc_library(
    name = "mesh",
    srcs = glob([
        "mesh/*.cpp",
        "mesh/submesh/*.cpp",
    ]),
    deps = [
        "config_hpp",
        "fem_hpp",
        "general_hpp",
        "linalg_hpp",
        "mesh_hpp",
    ],
)

### Headers ###################################################################

cc_library(
    name = "examples_hpp",
    srcs = glob(["examples/*.hpp"]),
)

cc_library(
    name = "general_hpp",
    srcs = glob(
        [
            "general/*.hpp",
            "general/*.h",
        ],
    ),
)

cc_library(
    name = "mfem_hpp",
    srcs = ["mfem.hpp"],
    deps = ["config_hpp"],
)

cc_library(
    name = "config_hpp",
    srcs = glob(["config/*.hpp"]),
)

cc_library(
    name = "fem_hpp",
    srcs = glob([
        "fem/*.hpp",
        "fem/*.h",
    ]),
    deps = [
        "fem_ceed_hpp",
        "fem_fe_hpp",
        "fem_integ_hpp",
        "fem_lor_hpp",
        # "fem_moonolith_hpp",
        "fem_qinterp_hpp",
        "fem_tmop_hpp",
    ],
)

cc_library(
    name = "fem_ceed_hpp",
    srcs = glob(["fem/ceed/**/*.hpp"]),
)

cc_library(
    name = "fem_moonolith_hpp",
    srcs = glob(["fem/moonolith/**/*.hpp"]),
)

cc_library(
    name = "fem_fe_hpp",
    srcs = glob(["fem/fe/*.hpp"]),
)

cc_library(
    name = "fem_integ_hpp",
    srcs = glob(["fem/integ/*.hpp"]),
)

cc_library(
    name = "fem_lor_hpp",
    srcs = glob(["fem/lor/*.hpp"]),
)

cc_library(
    name = "fem_qinterp_hpp",
    srcs = glob(["fem/qinterp/*.hpp"]),
)

cc_library(
    name = "fem_tmop_hpp",
    srcs = glob(["fem/tmop/*.hpp"]),
)

cc_library(
    name = "linalg_hpp",
    srcs = glob(["linalg/*.hpp"]),
    deps = [
        "linalg_batched_hpp",
        "linalg_simd_hpp",
    ],
)

cc_library(
    name = "linalg_batched_hpp",
    srcs = glob(["linalg/batched/*.hpp"]),
)

cc_library(
    name = "linalg_simd_hpp",
    srcs = glob(["linalg/simd/*.hpp"]),
)

cc_library(
    name = "mesh_hpp",
    srcs = glob(["mesh/*.hpp"]),
    deps = ["submesh_hpp"],
)

cc_library(
    name = "submesh_hpp",
    srcs = glob(["mesh/submesh/*.hpp"]),
)
