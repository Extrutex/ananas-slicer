#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <boost/beast/core/detail/base64.hpp>

#include <csetjmp>
#include <cstdio>
#include <jpeglib.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

// One "; thumbnail begin WxH LEN" ... "; thumbnail end" block as Moonraker reads it.
struct ThumbnailBlock
{
    unsigned int width{0};
    unsigned int height{0};
    size_t       declared_length{0};
    std::string  base64;
};

std::vector<ThumbnailBlock> thumbnail_blocks(const std::string &gcode)
{
    std::vector<ThumbnailBlock> blocks;
    static const std::regex begin_re(R"(^; thumbnail begin (\d+)x(\d+) (\d+)$)");
    std::istringstream      in(gcode);
    std::string             line;
    ThumbnailBlock         *open = nullptr;
    std::smatch             m;
    while (std::getline(in, line)) {
        if (open == nullptr) {
            if (std::regex_match(line, m, begin_re)) {
                blocks.push_back({unsigned(std::stoul(m[1])), unsigned(std::stoul(m[2])), size_t(std::stoul(m[3])), {}});
                open = &blocks.back();
            }
        } else if (line == "; thumbnail end") {
            open = nullptr;
        } else if (line.rfind("; ", 0) == 0) {
            open->base64 += line.substr(2);
        }
    }
    return blocks;
}

std::vector<unsigned char> decode_base64(const std::string &encoded)
{
    std::vector<unsigned char> out(boost::beast::detail::base64::decoded_size(encoded.size()));
    const auto result = boost::beast::detail::base64::decode(out.data(), encoded.data(), encoded.size());
    out.resize(result.first);
    return out;
}

uint32_t read_be32(const std::vector<unsigned char> &data, size_t offset)
{
    return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) | (uint32_t(data[offset + 2]) << 8) | uint32_t(data[offset + 3]);
}

// Stands in for the OpenGL renderer: an opaque square of the requested size.
ThumbnailsList solid_thumbnails(const ThumbnailsParams &params, std::vector<Vec2d> &requested)
{
    ThumbnailsList list;
    for (const Vec2d &size : params.sizes) {
        requested.push_back(size);
        ThumbnailData data;
        data.set(unsigned(size.x()), unsigned(size.y()));
        for (size_t i = 0; i < data.pixels.size(); i += 4) {
            data.pixels[i]     = 0xE0;
            data.pixels[i + 1] = 0x6C;
            data.pixels[i + 2] = 0x40;
            data.pixels[i + 3] = 0xFF;
        }
        list.push_back(std::move(data));
    }
    return list;
}

std::string export_with_thumbnails(const std::string &thumbnails, std::vector<Vec2d> &requested)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"thumbnails", thumbnails},
        {"thumbnails_format", "PNG"},
    });
    Print print;
    Model model;
    init_print({cube(20)}, print, model, config);
    print.set_status_silent();
    print.process();
    ScopedTemporaryFile temp(".gcode");
    print.export_gcode(temp.string(), nullptr,
                       [&requested](const ThumbnailsParams &params) { return solid_thumbnails(params, requested); });
    std::ifstream in(temp.string());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A thumbnail with varying colours, so the encoders produce more than a minimal file.
ThumbnailData gradient_thumbnail(unsigned int size)
{
    ThumbnailData data;
    data.set(size, size);
    for (unsigned int y = 0; y < size; ++y)
        for (unsigned int x = 0; x < size; ++x) {
            unsigned char *px = &data.pixels[4 * (y * size + x)];
            px[0] = (unsigned char) (x * 255 / size);
            px[1] = (unsigned char) (y * 255 / size);
            px[2] = (unsigned char) ((x * y) & 0xFF);
            px[3] = 0xFF;
        }
    return data;
}

struct JpegSize
{
    bool         ok{false};
    unsigned int width{0};
    unsigned int height{0};
};

// Decodes the whole file with libjpeg; a truncated or corrupt stream reports !ok.
JpegSize decode_jpeg(const unsigned char *bytes, size_t size)
{
    struct Error
    {
        jpeg_error_mgr mgr;
        jmp_buf        jump;
    };
    JpegSize                      result;
    jpeg_decompress_struct        info;
    Error                         err;
    std::vector<unsigned char>    row;
    info.err             = jpeg_std_error(&err.mgr);
    err.mgr.error_exit   = [](j_common_ptr cinfo) { longjmp(reinterpret_cast<Error *>(cinfo->err)->jump, 1); };
    err.mgr.emit_message = [](j_common_ptr cinfo, int level) {
        // A corrupt-data warning (level -1) makes the file invalid as well.
        if (level < 0)
            longjmp(reinterpret_cast<Error *>(cinfo->err)->jump, 1);
    };
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&info);
        return JpegSize{};
    }
    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, bytes, (unsigned long) size);
    jpeg_read_header(&info, TRUE);
    jpeg_start_decompress(&info);
    row.resize(size_t(info.output_width) * info.output_components);
    while (info.output_scanline < info.output_height) {
        unsigned char *rows[] = {row.data()};
        jpeg_read_scanlines(&info, rows, 1);
    }
    result = {true, info.output_width, info.output_height};
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    return result;
}

} // namespace

TEST_CASE("Configured PNG thumbnails are written as blocks Moonraker can decode", "[Thumbnails]")
{
    std::vector<Vec2d> requested;
    const std::string gcode = export_with_thumbnails("48x48/PNG, 300x300/PNG", requested);

    // The exporter asks the renderer once per configured size, in the configured order.
    const unsigned int expected_sizes[] = {48, 300};
    REQUIRE(requested.size() == 2);
    for (size_t i = 0; i < requested.size(); ++i) {
        CHECK_THAT(requested[i].x(), WithinAbs(expected_sizes[i], 1e-9));
        CHECK_THAT(requested[i].y(), WithinAbs(expected_sizes[i], 1e-9));
    }

    const std::vector<ThumbnailBlock> blocks = thumbnail_blocks(gcode);
    REQUIRE(blocks.size() == 2);
    for (size_t i = 0; i < blocks.size(); ++i) {
        const ThumbnailBlock &block = blocks[i];
        INFO("block " << i << ": " << block.width << "x" << block.height);
        CHECK(block.width == expected_sizes[i]);
        CHECK(block.height == expected_sizes[i]);
        // LEN in the header is the length of the Base64 text, which Moonraker checks before decoding.
        CHECK(block.base64.size() == block.declared_length);

        const std::vector<unsigned char> png = decode_base64(block.base64);
        REQUIRE(png.size() > 24);
        const unsigned char signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        CHECK(std::equal(std::begin(signature), std::end(signature), png.begin()));
        // The first chunk is IHDR; its width and height match the header line.
        CHECK(std::string(png.begin() + 12, png.begin() + 16) == "IHDR");
        CHECK(read_be32(png, 16) == block.width);
        CHECK(read_be32(png, 20) == block.height);
    }

    // The blocks sit in the file header, ahead of the first move.
    CHECK(gcode.find("; thumbnail end") < gcode.find("\nG1 "));
}

TEST_CASE("An empty thumbnails setting never calls the renderer", "[Thumbnails]")
{
    // The CLI creates its OpenGL context on the first render request; without sizes it must not be asked.
    std::vector<Vec2d> requested;
    const std::string gcode = export_with_thumbnails("", requested);
    CHECK(requested.empty());
    CHECK(thumbnail_blocks(gcode).empty());
}

TEST_CASE("JPG thumbnails are complete JPEG files at every size", "[Thumbnails]")
{
    // The encoder starts with an output buffer of width * height * 4 bytes; a JPEG larger than that
    // (1x1 always is) must still be returned whole.
    const unsigned int size = GENERATE(1u, 16u, 300u);
    INFO("thumbnail " << size << "x" << size);
    const auto compressed = GCodeThumbnails::compress_thumbnail(gradient_thumbnail(size), GCodeThumbnailsFormat::JPG);
    REQUIRE(compressed->data != nullptr);
    REQUIRE(compressed->size > 4);
    const auto *bytes = static_cast<const unsigned char *>(compressed->data);
    CHECK(bytes[0] == 0xFF);
    CHECK(bytes[1] == 0xD8);
    CHECK(bytes[compressed->size - 2] == 0xFF);
    CHECK(bytes[compressed->size - 1] == 0xD9);
    const JpegSize decoded = decode_jpeg(bytes, compressed->size);
    REQUIRE(decoded.ok);
    CHECK(decoded.width == size);
    CHECK(decoded.height == size);
}

TEST_CASE("QOI thumbnails carry the header and end marker of their size", "[Thumbnails]")
{
    const unsigned int size = GENERATE(1u, 16u, 300u);
    INFO("thumbnail " << size << "x" << size);
    const auto compressed = GCodeThumbnails::compress_thumbnail(gradient_thumbnail(size), GCodeThumbnailsFormat::QOI);
    REQUIRE(compressed->data != nullptr);
    REQUIRE(compressed->size >= 14 + 8);
    const auto *bytes = static_cast<const unsigned char *>(compressed->data);
    CHECK(std::string(bytes, bytes + 4) == "qoif");
    CHECK(read_be32(std::vector<unsigned char>(bytes, bytes + 12), 4) == size);
    CHECK(read_be32(std::vector<unsigned char>(bytes, bytes + 12), 8) == size);
    const unsigned char end_marker[] = {0, 0, 0, 0, 0, 0, 0, 1};
    CHECK(std::equal(std::begin(end_marker), std::end(end_marker), bytes + compressed->size - 8));
}
