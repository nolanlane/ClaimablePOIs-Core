set_xmakever("3.0.9")

includes("external/CommonLibSF")

set_project("ClaimablePOIs")
set_version("0.1.0")
set_license("GPL-3.0-or-later")
set_languages("c++23")
set_warnings("allextra", "error")
set_policy("build.warning", true)

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

target("ClaimablePOIs")
    add_rules("commonlibsf.plugin", {
        name = "ClaimablePOIs",
        author = "ClaimablePOIs contributors",
        description = "Conservative claim workflow for cleared exterior Starfield POIs",
        options = {
            sig_scanning = false,
            address_library = true,
            no_struct_use = false,
            layout_dependent = true
        }
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

target("ClaimablePOIsCoreTests")
    set_kind("binary")
    set_default(false)
    add_files("tests/CoreTests.cpp", "tests/FurnitureEnrollmentTests.cpp", "tests/ProceduralLifecycleTests.cpp", "tests/TeardownTests.cpp")
    add_includedirs("src")

target("ClaimablePOIsRefCountTests")
    set_kind("binary")
    set_default(false)
    add_deps("commonlibsf")
    add_files("tests/FormRefCountTests.cpp")
    set_pcxxheader("src/pch.h")
