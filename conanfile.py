import conans

class PividConan(conans.ConanFile):
    name, version = "pivid", "0.0"

    settings = "os", "compiler", "build_type", "arch"  # boilerplate
    requires = "ffmpeg/4.4", "fmt/8.0.1", "gflags/2.2.2"
    generators = "pkg_config"  # Used by the Meson build helper (below)

    default_options = {
        # Omit as much of ffmpeg as possible to minimize dependency build time
        "ffmpeg:postproc": False, **{
            f"ffmpeg:with_{lib}": False for lib in [
                "bzip2", "freetype", "libalsa", "libfdk_aac", "libiconv",
                "libmp3lame", "libvpx", "libwebp", "libx264", "libx265",
                "lzma", "openh264", "openjpeg", "opus", "programs", "pulse",
                "vaapi", "vdpau", "vorbis", "xcb", "zlib",
            ]
        }
    }

    def build(self):
        meson = conans.Meson(self)  # Uses the "pkg_config" generator (above)
        meson.configure()
        meson.build()