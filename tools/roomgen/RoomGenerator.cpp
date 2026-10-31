// LithiumRoomGen: paints the studio room that the kit renders sit in.
//
//   LithiumRoomGen <kit-renders-folder> [--preview <file.jpg>]
//
// It writes room.png into the folder, the same size as base.png. Every pixel
// is ray-cast through the same camera as assets/blender-kit.py, so the floor
// and walls share the kit's perspective and its floor shadow lands where it
// should, without needing Blender.
//
// The room is a box: floorboards with a rug under the kit, a back wall with a
// slatted wooden diffuser behind the kit and fabric acoustic panels either
// side, and side walls with more panels. It's lit by the kit's key and fill
// lights plus ceiling spots angled at the back wall, which throw soft pools
// of light onto it. Corners get a cheap ambient-occlusion darkening, and the
// floorboards get a blurred reflection of the walls.
//
// --preview also composites the kit renders over the room, to check the result.
//
// room.png is committed like the Blender renders, so the build only compiles
// this tool; run it by hand after changing it. It's deterministic.

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <thread>
#include <vector>

namespace
{

//==============================================================================
// Positions are in metres in Blender's world space: z up, the kick drum at
// the origin, and the camera looking along +y.
struct Vec
{
    double x = 0, y = 0, z = 0;

    Vec operator+ (Vec o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vec operator- (Vec o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vec operator* (double s) const { return { x * s, y * s, z * s }; }
    Vec operator* (Vec o) const { return { x * o.x, y * o.y, z * o.z }; }   // per component, for colours
    Vec& operator+= (Vec o) { return *this = *this + o; }
};

double dot (Vec a, Vec b)   { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross (Vec a, Vec b)    { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
Vec normalised (Vec v)      { return v * (1.0 / std::sqrt (dot (v, v))); }

// Linear-light RGB, where light adds up physically. Converted to sRGB at the end.
using Rgb = Vec;

Rgb mix (Rgb a, Rgb b, double t)  { return a + (b - a) * t; }

double smoothstep (double edge0, double edge1, double x)
{
    const auto t = std::clamp ((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

//==============================================================================
// Deterministic noise, so every run paints the same image.
double hash (int x, int y, int seed)
{
    auto h = (std::uint32_t) x * 0x8da6b343u ^ (std::uint32_t) y * 0xd8163841u ^ (std::uint32_t) seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (double) (h & 0xffffffu) / 16777216.0;
}

// Value noise: hashes on a grid, smoothly interpolated. Range [0, 1).
double noise (double x, double y, int seed)
{
    const auto xi = (int) std::floor (x), yi = (int) std::floor (y);
    const auto fx = smoothstep (0.0, 1.0, x - xi), fy = smoothstep (0.0, 1.0, y - yi);

    return std::lerp (std::lerp (hash (xi, yi, seed),     hash (xi + 1, yi, seed),     fx),
                      std::lerp (hash (xi, yi + 1, seed), hash (xi + 1, yi + 1, seed), fx), fy);
}

// Octaves of noise at doubling frequencies, for natural-looking variation.
double fbm (double x, double y, int seed, int octaves = 4)
{
    double sum = 0, amplitude = 0.5, total = 0;

    for (int i = 0; i < octaves; ++i)
    {
        sum += noise (x, y, seed + i) * amplitude;
        total += amplitude;
        amplitude *= 0.5;
        x *= 2.03;
        y *= 2.03;
    }

    return sum / total;
}

//==============================================================================
// Must match the camera in assets/blender-kit.py: its position and target, a
// 31 mm lens, and Blender's default 36 mm sensor fitted to the image width.
struct Camera
{
    Vec position { -0.12, -3.4, 2.05 };
    Vec forward, right, up;
    double lensMm = 31.0, sensorWidthMm = 36.0;
    int width, height;

    Camera (int w, int h) : width (w), height (h)
    {
        const Vec target { -0.12, 0.25, 0.74 };
        forward = normalised (target - position);
        right = normalised (cross (forward, { 0, 0, 1 }));   // Blender's track-to keeps the camera upright
        up = cross (right, forward);
    }

    // The ray through a point on the image, in pixels from the top left.
    Vec rayThrough (double px, double py) const
    {
        const auto sensorX = (px / width - 0.5) * sensorWidthMm;
        const auto sensorY = (0.5 - py / height) * sensorWidthMm * height / width;
        return normalised (forward * lensMm + right * sensorX + up * sensorY);
    }
};

//==============================================================================
namespace room
{
    constexpr double backWall = 2.1, leftWall = -2.6, rightWall = 2.4, ceiling = 3.2;

    // The rug under the kit, as floor coordinates.
    constexpr double rugLeft = -1.6, rugRight = 1.3, rugFront = -0.95, rugBack = 1.05;
}

enum class Surface { floor, ceiling, backWall, leftWall, rightWall };

struct Hit
{
    Surface surface;
    Vec point, normal;
    double distance;
};

// The camera is inside the room's box, so a ray leaves through whichever
// plane it reaches first.
Hit trace (Vec origin, Vec dir)
{
    Hit nearest { Surface::floor, {}, {}, std::numeric_limits<double>::max() };

    auto consider = [&] (Surface surface, double t, Vec normal)
    {
        if (t > 1.0e-6 && t < nearest.distance)
            nearest = { surface, origin + dir * t, normal, t };
    };

    if (dir.z < 0) consider (Surface::floor,     -origin.z / dir.z,                       { 0, 0, 1 });
    if (dir.z > 0) consider (Surface::ceiling,   (room::ceiling - origin.z) / dir.z,      { 0, 0, -1 });
    if (dir.y > 0) consider (Surface::backWall,  (room::backWall - origin.y) / dir.y,     { 0, -1, 0 });
    if (dir.x < 0) consider (Surface::leftWall,  (room::leftWall - origin.x) / dir.x,     { 1, 0, 0 });
    if (dir.x > 0) consider (Surface::rightWall, (room::rightWall - origin.x) / dir.x,    { -1, 0, 0 });

    return nearest;
}

//==============================================================================
struct Material
{
    Rgb albedo;
    double gloss;       // strength of the floor's blurred reflection
    double occlusion;   // darkening from nearby geometry, 1 = none
};

struct Rect
{
    double left, right, bottom, top;

    // Signed distance from a point to the rectangle's edge: negative inside.
    double distanceTo (double u, double v) const
    {
        const auto dx = std::max (left - u, u - right), dy = std::max (bottom - v, v - top);

        if (dx > 0 || dy > 0)
            return std::hypot (std::max (dx, 0.0), std::max (dy, 0.0));

        return std::max (dx, dy);
    }
};

// Wall features in wall coordinates: u runs along the wall (x on the back
// wall, y on the side walls) and v is the height. Tops run out of frame.
const Rect diffuser { -1.2, 1.0, 0.32, 2.6 };
const std::vector<Rect> backPanels { { -1.95, -1.35, 0.45, 2.6 }, { 1.15, 1.75, 0.45, 2.6 } };
const std::vector<Rect> sidePanels { { 0.1, 0.72, 0.45, 2.6 }, { 0.86, 1.48, 0.45, 2.6 } };

// Corners and edges get less light. A cheap stand-in for ambient occlusion.
double occlusionNear (double distance, double strength = 0.5, double reach = 0.25)
{
    return 1.0 - strength * std::exp (-distance / reach);
}

Material floorMaterial (Vec p)
{
    auto occlusion = occlusionNear (room::backWall - p.y) * occlusionNear (p.x - room::leftWall)
                   * occlusionNear (room::rightWall - p.x);

    // A broad soft darkening under the kit, which blocks light from the floor.
    // base.png adds the kit's sharper contact shadows on top.
    const auto kx = (p.x + 0.15) / 1.25, ky = (p.y - 0.25) / 0.75;
    occlusion *= 1.0 - 0.35 * std::exp (-(kx * kx + ky * ky) * 1.5);

    // How far inside the rug's edge the point is; negative outside it.
    const auto insideRug = std::min ({ p.x - room::rugLeft, room::rugRight - p.x, p.y - room::rugFront, room::rugBack - p.y });

    if (insideRug > 0)
    {
        const Rgb field { 0.030, 0.034, 0.043 }, border { 0.052, 0.060, 0.074 }, binding { 0.016, 0.018, 0.022 };
        const auto pile = 0.8 + 0.25 * fbm (p.x * 90, p.y * 90, 11, 2) + 0.2 * (fbm (p.x * 2.5, p.y * 2.5, 13) - 0.5);

        auto albedo = mix (field, border, smoothstep (0.065, 0.075, insideRug) * (1.0 - smoothstep (0.15, 0.16, insideRug)));
        albedo = mix (binding, albedo, smoothstep (0.015, 0.025, insideRug));
        return { albedo * pile, 0.0, occlusion };
    }

    // The rug stands a little proud of the floor and shades the boards next to it.
    occlusion *= occlusionNear (-insideRug, 0.35, 0.03);

    // Boards run away from the camera, with joints staggered row by row.
    constexpr double boardWidth = 0.15, boardLength = 1.9;
    const auto row = (int) std::floor (p.x / boardWidth);
    const auto along = p.y + hash (row, 0, 3) * boardLength;
    const auto board = (int) std::floor (along / boardLength);
    const auto across = p.x / boardWidth - row;
    const auto lengthwise = along / boardLength - board;

    const auto sideGap = std::min (across, 1.0 - across) * boardWidth;
    const auto endGap = std::min (lengthwise, 1.0 - lengthwise) * boardLength;
    const auto joint = std::min (smoothstep (0.0008, 0.0022, sideGap), smoothstep (0.001, 0.003, endGap));

    // Grain: noise stretched along the board, with fine streaks through it.
    const auto grain = fbm (p.x * 70 + row * 7.3, p.y * 3.0 + board * 3.1, 7);
    const auto streaks = 0.5 + 0.5 * std::sin ((p.x * 900 + grain * 40) + row * 2.0);
    const auto tint = 0.75 + 0.5 * hash (row, board, 5);

    const Rgb walnut { 0.060, 0.042, 0.031 };
    const auto albedo = walnut * (tint * (0.7 + 0.5 * grain + 0.12 * streaks)) * (0.25 + 0.75 * joint);

    return { albedo, 0.55 * joint, occlusion };
}

Material wallMaterial (Surface wall, Vec p)
{
    const auto isBack = wall == Surface::backWall;
    const auto u = isBack ? p.x : room::backWall - p.y;   // side walls measure from the back corner
    const auto v = p.z;

    auto occlusion = occlusionNear (v, 0.55, 0.3);

    if (isBack)
        occlusion *= occlusionNear (p.x - room::leftWall, 0.45) * occlusionNear (room::rightWall - p.x, 0.45);
    else
        occlusion *= occlusionNear (room::backWall - p.y, 0.45);

    if (v < 0.1)   // skirting board
        return { Rgb { 0.020, 0.021, 0.024 }, 0.0, occlusion };

    const Rgb paint { 0.040, 0.050, 0.064 };   // steel blue, so the walls sit with the UI palette

    // Mounted features stand off the wall; the key light comes from the upper
    // left, so their shadows fall down and to the right of them.
    auto shadowFrom = [&] (const Rect& r, double distance)
    {
        const auto shifted = r.distanceTo (u - (isBack ? 0.03 : 0.0), v + 0.04);
        return (1.0 - 0.45 * (1.0 - smoothstep (-0.01, 0.06, shifted))) * occlusionNear (distance, 0.3, 0.025);
    };

    if (isBack)
    {
        const auto d = diffuser.distanceTo (u, v);

        if (d < 0)
        {
            // Vertical wooden slats over a dark cavity.
            constexpr double pitch = 0.07, slat = 0.048;
            const auto index = (int) std::floor ((u - diffuser.left) / pitch);
            const auto across = (u - diffuser.left) - index * pitch;
            const auto onSlat = smoothstep (0.0, 0.002, across) * (1.0 - smoothstep (slat - 0.002, slat, across));
            const auto grain = fbm (u * 120 + index * 5.7, v * 4.0, 21);

            const Rgb oak { 0.105, 0.072, 0.050 }, cavity { 0.014, 0.015, 0.018 };
            const auto wood = oak * ((0.85 + 0.3 * hash (index, 0, 23)) * (0.8 + 0.35 * grain));
            const auto edge = occlusionNear (std::min (across, slat - across), 0.35, 0.006);

            return { mix (cavity, wood * edge, onSlat), 0.0, occlusion * occlusionNear (-d, 0.3, 0.02) };
        }

        occlusion *= shadowFrom (diffuser, d);
    }

    for (const auto& panel : isBack ? backPanels : sidePanels)
    {
        const auto d = panel.distanceTo (u, v);

        if (d < 0)
        {
            // Fabric-wrapped panel; the wrapped edge turns away from the light.
            const Rgb fabric { 0.040, 0.052, 0.068 };
            const auto weave = 0.88 + 0.16 * fbm (u * 160, v * 160, 31, 2) + 0.1 * (fbm (u * 3, v * 3, 33) - 0.5);
            return { fabric * weave, 0.0, occlusion * occlusionNear (-d, 0.4, 0.012) };
        }

        occlusion *= shadowFrom (panel, d);
    }

    return { paint * (0.95 + 0.1 * fbm (u * 6, v * 6, 41)), 0.0, occlusion };
}

Material materialAt (const Hit& hit)
{
    switch (hit.surface)
    {
        case Surface::floor:     return floorMaterial (hit.point);
        case Surface::ceiling:   return { Rgb { 0.015, 0.016, 0.018 }, 0.0, 1.0 };
        case Surface::backWall:
        case Surface::leftWall:
        case Surface::rightWall: return wallMaterial (hit.surface, hit.point);
    }

    return {};
}

//==============================================================================
struct Spotlight
{
    Vec position, direction;
    double cosOuter, cosInner;   // the cone fades out between these angles
    Rgb colour;
};

Spotlight spot (Vec position, Vec aimAt, double innerDegrees, double outerDegrees, Rgb colour)
{
    return { position, normalised (aimAt - position),
             std::cos (juce::degreesToRadians (outerDegrees)), std::cos (juce::degreesToRadians (innerDegrees)), colour };
}

const Rgb spotColour { 1.0, 0.76, 0.50 };

const std::vector<Spotlight> lights {
    // The kit's key and fill lights, so the room is lit the way the kit is.
    spot ({ -1.6, -2.2, 3.0 }, { 0, 0.2, 0.7 }, 40, 75, Rgb { 1.0, 0.96, 0.90 } * 3.2),
    spot ({ 2.2, -1.6, 1.8 }, { 0, 0.2, 0.7 }, 40, 75, Rgb { 0.80, 0.90, 1.0 } * 1.8),

    // Ceiling spots angled at the back wall: a pool on each panel and one on
    // the slats behind the kit. Aiming them at the wall rather than straight
    // down keeps most of their light off the floor.
    spot ({ -1.65, 0.8, 3.1 }, { -1.65, room::backWall, 1.0 }, 10, 22, spotColour * 7.0),
    spot ({ -0.10, 0.8, 3.1 }, { -0.10, room::backWall, 1.1 }, 12, 26, spotColour * 9.0),
    spot ({ 1.45, 0.8, 3.1 },  { 1.45, room::backWall, 1.0 },  10, 22, spotColour * 7.0),
};

const Rgb ambient { 0.022, 0.026, 0.032 };

Rgb lightArriving (Vec p, Vec normal)
{
    auto total = ambient;

    for (const auto& light : lights)
    {
        const auto toLight = light.position - p;
        const auto distanceSquared = dot (toLight, toLight);
        const auto l = toLight * (1.0 / std::sqrt (distanceSquared));
        const auto facing = dot (normal, l);

        if (facing <= 0)
            continue;

        const auto cone = smoothstep (light.cosOuter, light.cosInner, -dot (l, light.direction));
        total += light.colour * (facing * cone / distanceSquared);
    }

    return total;
}

Rgb shade (const Hit& hit, Vec dir, int seed, bool withReflection)
{
    const auto material = materialAt (hit);
    auto colour = material.albedo * lightArriving (hit.point, hit.normal) * material.occlusion;

    if (withReflection && material.gloss > 0)
    {
        // A few jittered mirror rays give a soft, blurred reflection.
        constexpr int rays = 4;
        constexpr double roughness = 0.07;
        const auto mirror = dir - hit.normal * (2.0 * dot (dir, hit.normal));
        const auto fresnel = 0.04 + 0.96 * std::pow (1.0 + dot (dir, hit.normal), 5.0);

        Rgb reflected;

        for (int i = 0; i < rays; ++i)
        {
            const Vec jitter { hash (seed, i, 51) - 0.5, hash (seed, i, 52) - 0.5, hash (seed, i, 53) - 0.5 };
            const auto ray = normalised (mirror + jitter * (2.0 * roughness));
            reflected += shade (trace (hit.point, ray), ray, seed, false);
        }

        colour += reflected * (fresnel * material.gloss * material.occlusion / rays);
    }

    return colour;
}

//==============================================================================
// Narkowicz's fit of the ACES filmic curve: rolls highlights off gently.
double filmic (double x)
{
    return std::clamp ((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

double toSrgb (double linear)
{
    return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow (linear, 1.0 / 2.4) - 0.055;
}

juce::Image paintRoom (int width, int height)
{
    const Camera camera (width, height);
    std::vector<Rgb> pixels ((size_t) width * (size_t) height);

    constexpr int grid = 3;   // 3x3 samples per pixel, for antialiasing

    auto renderRows = [&] (int firstRow, int step)
    {
        for (int y = firstRow; y < height; y += step)
        {
            for (int x = 0; x < width; ++x)
            {
                Rgb sum;

                for (int s = 0; s < grid * grid; ++s)
                {
                    const auto sx = x * grid + s % grid, sy = y * grid + s / grid;
                    const auto dir = camera.rayThrough ((sx + 0.5) / grid, (sy + 0.5) / grid);
                    sum += shade (trace (camera.position, dir), dir, sx * 7919 + sy, true);
                }

                pixels[(size_t) y * (size_t) width + (size_t) x] = sum * (1.0 / (grid * grid));
            }
        }
    };

    {
        // Rows are independent, so each thread takes every Nth row and writes
        // only its own pixels; nothing is shared, so nothing needs locking.
        // std::jthread joins when it's destroyed, at the end of this block.
        const auto threadCount = (int) std::max (1u, std::thread::hardware_concurrency());
        std::vector<std::jthread> workers;

        for (int t = 0; t < threadCount; ++t)
            workers.emplace_back (renderRows, t, threadCount);
    }

    // Tone-map, then grade toward the stage's edge colour (#0B0F13) at the
    // edges, so the room melts into the header and strips around it.
    constexpr double exposure = 2.6;
    const Rgb edge { 11 / 255.0, 15 / 255.0, 19 / 255.0 };

    juce::Image image (juce::Image::RGB, width, height, false);
    const juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const auto linear = pixels[(size_t) y * (size_t) width + (size_t) x] * exposure;
            Rgb srgb { toSrgb (filmic (linear.x)), toSrgb (filmic (linear.y)), toSrgb (filmic (linear.z)) };

            const auto nx = (x + 0.5) / width * 2.0 - 1.0, ny = (y + 0.5) / height * 2.0 - 1.0;
            const auto vignette = smoothstep (0.75, 1.45, std::sqrt (nx * nx * 0.85 + ny * ny * 0.35))
                                + 0.3 * smoothstep (0.7, 1.0, ny) + 0.25 * smoothstep (-0.85, -1.0, ny);
            srgb = mix (srgb, edge, std::min (vignette, 0.8));

            // A little noise breaks up banding in the smooth gradients.
            const auto dither = (hash (x, y, 99) - 0.5) / 255.0;

            auto channel = [&] (double c) { return (juce::uint8) std::clamp (std::lround ((c + dither) * 255.0), 0L, 255L); };
            data.setPixelColour (x, y, juce::Colour (channel (srgb.x), channel (srgb.y), channel (srgb.z)));
        }
    }

    return image;
}

//==============================================================================
bool writeImage (const juce::Image& image, const juce::File& file, juce::ImageFileFormat& format)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    return stream.openedOk() && format.writeImageToStream (image, stream);
}

// Draws the kit renders over the room the way KitStage does, at render scale.
juce::Image composite (const juce::File& folder, const juce::Image& room)
{
    auto canvas = room.convertedToFormat (juce::Image::ARGB);   // a new image, so the room isn't drawn on
    juce::Graphics g (canvas);

    const auto layout = juce::JSON::parse (folder.getChildFile ("layout.json"));

    if (const auto* order = layout["drawOrderBackToFront"].getArray())
    {
        for (const auto& name : *order)
        {
            const auto& rect = layout["pieces"][juce::Identifier (name.toString())];
            g.drawImageAt (juce::ImageFileFormat::loadFrom (folder.getChildFile (name.toString() + ".png")),
                           (int) rect["x"], (int) rect["y"]);
        }
    }

    return canvas;
}

} // namespace

int main (int argc, char* argv[])
{
    if (argc != 2 && ! (argc == 4 && juce::String (argv[2]) == "--preview"))
    {
        std::cerr << "usage: LithiumRoomGen <kit-renders-folder> [--preview <file.jpg>]" << std::endl;
        return 1;
    }

    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const auto folder = cwd.getChildFile (argv[1]);
    const auto base = juce::ImageFileFormat::loadFrom (folder.getChildFile ("base.png"));

    if (! base.isValid())
    {
        std::cerr << "No base.png in " << folder.getFullPathName() << std::endl;
        return 1;
    }

    const auto room = paintRoom (base.getWidth(), base.getHeight());
    const auto roomFile = folder.getChildFile ("room.png");

    juce::PNGImageFormat png;

    if (! writeImage (room, roomFile, png))
    {
        std::cerr << "Couldn't write " << roomFile.getFullPathName() << std::endl;
        return 1;
    }

    std::cout << "Wrote " << roomFile.getFullPathName() << " (" << room.getWidth() << "x" << room.getHeight() << ")" << std::endl;

    if (argc == 4)
    {
        const auto previewFile = cwd.getChildFile (argv[3]);
        juce::JPEGImageFormat jpeg;
        jpeg.setQuality (0.92f);

        if (! writeImage (composite (folder, room), previewFile, jpeg))
        {
            std::cerr << "Couldn't write " << previewFile.getFullPathName() << std::endl;
            return 1;
        }

        std::cout << "Wrote " << previewFile.getFullPathName() << std::endl;
    }

    return 0;
}
