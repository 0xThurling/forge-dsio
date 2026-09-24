return {
    project = {
        name = "dsio",
        type = "library",
        standard = "20",
        install_headers = true,
    },
    build = {
        presets = { "warnings" },
    },
    testing = {
        enabled = true,
        framework = "gtest",
        benchmark = true,
    },
    dependencies = {
        direct = {
            forgefp = {
                path = "../fp",
                target = "forgefp",
                export = {
                    package = "forgefp",
                    target = "forgefp::forgefp",
                },
            },
            googletest = {
                git = "https://github.com/google/googletest.git",
                tag = "v1.14.0",
            },
        },
        conan = {},
        pkgconfig = { "liburing" },
    },
    resources = {
        files = {},
    },
    scripts = {},
    features = {
    },
}
