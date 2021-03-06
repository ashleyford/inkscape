/*
 * Helper functions to use cairo with inkscape
 *
 * Copyright (C) 2007 bulia byak
 * Copyright (C) 2008 Johan Engelen
 *
 * Released under GNU GPL
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include "display/cairo-utils.h"

#include <stdexcept>
#include <2geom/pathvector.h>
#include <2geom/bezier-curve.h>
#include <2geom/hvlinesegment.h>
#include <2geom/affine.h>
#include <2geom/point.h>
#include <2geom/path.h>
#include <2geom/transforms.h>
#include <2geom/sbasis-to-bezier.h>
#include "color.h"
#include "helper/geom-curves.h"

namespace Inkscape {

CairoGroup::CairoGroup(cairo_t *_ct) : ct(_ct), pushed(false) {}
CairoGroup::~CairoGroup() {
    if (pushed) {
        cairo_pattern_t *p = cairo_pop_group(ct);
        cairo_pattern_destroy(p);
    }
}
void CairoGroup::push() {
    cairo_push_group(ct);
    pushed = true;
}
void CairoGroup::push_with_content(cairo_content_t content) {
    cairo_push_group_with_content(ct, content);
    pushed = true;
}
cairo_pattern_t *CairoGroup::pop() {
    if (pushed) {
        cairo_pattern_t *ret = cairo_pop_group(ct);
        pushed = false;
        return ret;
    } else {
        throw std::logic_error("Cairo group popped without pushing it first");
    }
}
Cairo::RefPtr<Cairo::Pattern> CairoGroup::popmm() {
    if (pushed) {
        cairo_pattern_t *ret = cairo_pop_group(ct);
        Cairo::RefPtr<Cairo::Pattern> retmm(new Cairo::Pattern(ret, true));
        pushed = false;
        return retmm;
    } else {
        throw std::logic_error("Cairo group popped without pushing it first");
    }
}
void CairoGroup::pop_to_source() {
    if (pushed) {
        cairo_pop_group_to_source(ct);
        pushed = false;
    }
}

CairoContext::CairoContext(cairo_t *obj, bool ref)
    : Cairo::Context(obj, ref)
{}

void CairoContext::transform(Geom::Affine const &m)
{
    cairo_matrix_t cm;
    cm.xx = m[0];
    cm.xy = m[2];
    cm.x0 = m[4];
    cm.yx = m[1];
    cm.yy = m[3];
    cm.y0 = m[5];
    cairo_transform(cobj(), &cm);
}

void CairoContext::set_source_rgba32(guint32 color)
{
    double red = SP_RGBA32_R_F(color);
    double gre = SP_RGBA32_G_F(color);
    double blu = SP_RGBA32_B_F(color);
    double alp = SP_RGBA32_A_F(color);
    cairo_set_source_rgba(cobj(), red, gre, blu, alp);
}

void CairoContext::append_path(Geom::PathVector const &pv)
{
    feed_pathvector_to_cairo(cobj(), pv);
}

Cairo::RefPtr<CairoContext> CairoContext::create(Cairo::RefPtr<Cairo::Surface> const &target)
{
    cairo_t *ct = cairo_create(target->cobj());
    Cairo::RefPtr<CairoContext> ret(new CairoContext(ct, true));
    return ret;
}

} // namespace Inkscape

/*
 * Can be called recursively.
 * If optimize_stroke == false, the view Rect is not used.
 */
static void
feed_curve_to_cairo(cairo_t *cr, Geom::Curve const &c, Geom::Affine const & trans, Geom::Rect view, bool optimize_stroke)
{
    if( is_straight_curve(c) )
    {
        Geom::Point end_tr = c.finalPoint() * trans;
        if (!optimize_stroke) {
            cairo_line_to(cr, end_tr[0], end_tr[1]);
        } else {
            Geom::Rect swept(c.initialPoint()*trans, end_tr);
            if (swept.intersects(view)) {
                cairo_line_to(cr, end_tr[0], end_tr[1]);
            } else {
                cairo_move_to(cr, end_tr[0], end_tr[1]);
            }
        }
    }
    else if(Geom::QuadraticBezier const *quadratic_bezier = dynamic_cast<Geom::QuadraticBezier const*>(&c)) {
        std::vector<Geom::Point> points = quadratic_bezier->points();
        points[0] *= trans;
        points[1] *= trans;
        points[2] *= trans;
        Geom::Point b1 = points[0] + (2./3) * (points[1] - points[0]);
        Geom::Point b2 = b1 + (1./3) * (points[2] - points[0]);
        if (!optimize_stroke) {
            cairo_curve_to(cr, b1[0], b1[1], b2[0], b2[1], points[2][0], points[2][1]);
        } else {
            Geom::Rect swept(points[0], points[2]);
            swept.expandTo(points[1]);
            if (swept.intersects(view)) {
                cairo_curve_to(cr, b1[0], b1[1], b2[0], b2[1], points[2][0], points[2][1]);
            } else {
                cairo_move_to(cr, points[2][0], points[2][1]);
            }
        }
    }
    else if(Geom::CubicBezier const *cubic_bezier = dynamic_cast<Geom::CubicBezier const*>(&c)) {
        std::vector<Geom::Point> points = cubic_bezier->points();
        //points[0] *= trans; // don't do this one here for fun: it is only needed for optimized strokes
        points[1] *= trans;
        points[2] *= trans;
        points[3] *= trans;
        if (!optimize_stroke) {
            cairo_curve_to(cr, points[1][0], points[1][1], points[2][0], points[2][1], points[3][0], points[3][1]);
        } else {
            points[0] *= trans;  // didn't transform this point yet
            Geom::Rect swept(points[0], points[3]);
            swept.expandTo(points[1]);
            swept.expandTo(points[2]);
            if (swept.intersects(view)) {
                cairo_curve_to(cr, points[1][0], points[1][1], points[2][0], points[2][1], points[3][0], points[3][1]);
            } else {
                cairo_move_to(cr, points[3][0], points[3][1]);
            }
        }
    }
//    else if(Geom::SVGEllipticalArc const *svg_elliptical_arc = dynamic_cast<Geom::SVGEllipticalArc *>(c)) {
//        //TODO: get at the innards and spit them out to cairo
//    }
    else {
        //this case handles sbasis as well as all other curve types
        Geom::Path sbasis_path = Geom::cubicbezierpath_from_sbasis(c.toSBasis(), 0.1);

        //recurse to convert the new path resulting from the sbasis to svgd
        for(Geom::Path::iterator iter = sbasis_path.begin(); iter != sbasis_path.end(); ++iter) {
            feed_curve_to_cairo(cr, *iter, trans, view, optimize_stroke);
        }
    }
}


/** Feeds path-creating calls to the cairo context translating them from the Path */
static void
feed_path_to_cairo (cairo_t *ct, Geom::Path const &path)
{
    if (path.empty())
        return;

    cairo_move_to(ct, path.initialPoint()[0], path.initialPoint()[1] );

    for(Geom::Path::const_iterator cit = path.begin(); cit != path.end_open(); ++cit) {
        feed_curve_to_cairo(ct, *cit, Geom::identity(), Geom::Rect(), false); // optimize_stroke is false, so the view rect is not used
    }

    if (path.closed()) {
        cairo_close_path(ct);
    }
}

/** Feeds path-creating calls to the cairo context translating them from the Path, with the given transform and shift */
static void
feed_path_to_cairo (cairo_t *ct, Geom::Path const &path, Geom::Affine trans, Geom::OptRect area, bool optimize_stroke, double stroke_width)
{
    if (!area)
        return;
    if (path.empty())
        return;

    // Transform all coordinates to coords within "area"
    Geom::Point shift = area->min();
    Geom::Rect view = *area;
    view.expandBy (stroke_width);
    view = view * (Geom::Affine)Geom::Translate(-shift);
    //  Pass transformation to feed_curve, so that we don't need to create a whole new path.
    Geom::Affine transshift(trans * Geom::Translate(-shift));

    Geom::Point initial = path.initialPoint() * transshift;
    cairo_move_to(ct, initial[0], initial[1] );

    for(Geom::Path::const_iterator cit = path.begin(); cit != path.end_open(); ++cit) {
        feed_curve_to_cairo(ct, *cit, transshift, view, optimize_stroke);
    }

    if (path.closed()) {
        if (!optimize_stroke) {
            cairo_close_path(ct);
        } else {
            cairo_line_to(ct, initial[0], initial[1]);
            /* We cannot use cairo_close_path(ct) here because some parts of the path may have been
               clipped and not drawn (maybe the before last segment was outside view area), which 
               would result in closing the "subpath" after the last interruption, not the entire path.

               However, according to cairo documentation:
               The behavior of cairo_close_path() is distinct from simply calling cairo_line_to() with the equivalent coordinate
               in the case of stroking. When a closed sub-path is stroked, there are no caps on the ends of the sub-path. Instead,
               there is a line join connecting the final and initial segments of the sub-path. 

               The correct fix will be possible when cairo introduces methods for moving without
               ending/starting subpaths, which we will use for skipping invisible segments; then we
               will be able to use cairo_close_path here. This issue also affects ps/eps/pdf export,
               see bug 168129
            */
        }
    }
}

/** Feeds path-creating calls to the cairo context translating them from the PathVector, with the given transform and shift
 *  One must have done cairo_new_path(ct); before calling this function. */
void
feed_pathvector_to_cairo (cairo_t *ct, Geom::PathVector const &pathv, Geom::Affine trans, Geom::OptRect area, bool optimize_stroke, double stroke_width)
{
    if (!area)
        return;
    if (pathv.empty())
        return;

    for(Geom::PathVector::const_iterator it = pathv.begin(); it != pathv.end(); ++it) {
        feed_path_to_cairo(ct, *it, trans, area, optimize_stroke, stroke_width);
    }
}

/** Feeds path-creating calls to the cairo context translating them from the PathVector
 *  One must have done cairo_new_path(ct); before calling this function. */
void
feed_pathvector_to_cairo (cairo_t *ct, Geom::PathVector const &pathv)
{
    if (pathv.empty())
        return;

    for(Geom::PathVector::const_iterator it = pathv.begin(); it != pathv.end(); ++it) {
        feed_path_to_cairo(ct, *it);
    }
}

void
ink_cairo_set_source_rgba32(cairo_t *ct, guint32 rgba)
{
    cairo_set_source_rgba(ct, SP_RGBA32_R_F(rgba), SP_RGBA32_G_F(rgba), SP_RGBA32_B_F(rgba), SP_RGBA32_A_F(rgba));
}

void
ink_cairo_set_source_color(cairo_t *ct, SPColor const &c, double opacity)
{
    cairo_set_source_rgba(ct, c.v.c[0], c.v.c[1], c.v.c[2], opacity);
}

void ink_matrix_to_2geom(Geom::Affine &m, cairo_matrix_t const &cm)
{
    m[0] = cm.xx;
    m[2] = cm.xy;
    m[4] = cm.x0;
    m[1] = cm.yx;
    m[3] = cm.yy;
    m[5] = cm.y0;
}

void ink_matrix_to_cairo(cairo_matrix_t &cm, Geom::Affine const &m)
{
    cm.xx = m[0];
    cm.xy = m[2];
    cm.x0 = m[4];
    cm.yx = m[1];
    cm.yy = m[3];
    cm.y0 = m[5];
}

void
ink_cairo_transform(cairo_t *ct, Geom::Affine const &m)
{
    cairo_matrix_t cm;
    ink_matrix_to_cairo(cm, m);
    cairo_transform(ct, &cm);
}

void
ink_cairo_pattern_set_matrix(cairo_pattern_t *cp, Geom::Affine const &m)
{
    cairo_matrix_t cm;
    ink_matrix_to_cairo(cm, m);
    cairo_pattern_set_matrix(cp, &cm);
}

void
ink_cairo_set_source_argb32_pixbuf(cairo_t *ct, GdkPixbuf *pb, double x, double y)
{
    cairo_surface_t *pbs = ink_cairo_surface_create_for_argb32_pixbuf(pb);
    cairo_set_source_surface(ct, pbs, x, y);
    cairo_surface_destroy(pbs);
}

cairo_surface_t *
ink_cairo_surface_create_for_argb32_pixbuf(GdkPixbuf *pb)
{
    guchar *data = gdk_pixbuf_get_pixels(pb);
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);

    cairo_surface_t *pbs = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32, w, h, stride);
    return pbs;
}

/**
 * Cleanup function for GdkPixbuf.
 * This function should be passed as the GdkPixbufDestroyNotify parameter
 * to gdk_pixbuf_new_from_data when creating a GdkPixbuf backed by
 * a Cairo surface.
 */
void ink_cairo_pixbuf_cleanup(guchar * /*pixels*/, void *data)
{
    cairo_surface_t *surface = reinterpret_cast<cairo_surface_t*>(data);
    cairo_surface_destroy(surface);
}

/**
 * Create an exact copy of a surface.
 * Creates a surface that has the same type, content type, dimensions and contents
 * as the specified surface.
 */
cairo_surface_t *
ink_cairo_surface_copy(cairo_surface_t *s)
{
    cairo_surface_t *ns = ink_cairo_surface_create_identical(s);

    if (cairo_surface_get_type(s) == CAIRO_SURFACE_TYPE_IMAGE) {
        // use memory copy instead of using a Cairo context
        cairo_surface_flush(s);
        int stride = cairo_image_surface_get_stride(s);
        int h = cairo_image_surface_get_height(s);
        memcpy(cairo_image_surface_get_data(ns), cairo_image_surface_get_data(s), stride * h);
        cairo_surface_mark_dirty(ns);
    } else {
        // generic implementation
        cairo_t *ct = cairo_create(ns);
        cairo_set_source_surface(ct, s, 0, 0);
        cairo_set_operator(ct, CAIRO_OPERATOR_SOURCE);
        cairo_paint(ct);
        cairo_destroy(ct);
    }

    return ns;
}

/**
 * Create a surface that differs only in pixel content.
 * Creates a surface that has the same type, content type and dimensions
 * as the specified surface. Pixel contents are not copied.
 */
cairo_surface_t *
ink_cairo_surface_create_identical(cairo_surface_t *s)
{
    cairo_surface_t *ns = ink_cairo_surface_create_same_size(s, cairo_surface_get_content(s));
    return ns;
}

cairo_surface_t *
ink_cairo_surface_create_same_size(cairo_surface_t *s, cairo_content_t c)
{
    cairo_surface_t *ns = cairo_surface_create_similar(s, c,
        ink_cairo_surface_get_width(s), ink_cairo_surface_get_height(s));
    return ns;
}

/**
 * Extract the alpha channel into a new surface.
 * Creates a surface with a content type of CAIRO_CONTENT_ALPHA that contains
 * the alpha values of pixels from @a s.
 */
cairo_surface_t *
ink_cairo_extract_alpha(cairo_surface_t *s)
{
    cairo_surface_t *alpha = ink_cairo_surface_create_same_size(s, CAIRO_CONTENT_ALPHA);

    cairo_t *ct = cairo_create(alpha);
    cairo_set_source_surface(ct, s, 0, 0);
    cairo_set_operator(ct, CAIRO_OPERATOR_SOURCE);
    cairo_paint(ct);
    cairo_destroy(ct);

    return alpha;
}

cairo_surface_t *
ink_cairo_surface_create_output(cairo_surface_t *image, cairo_surface_t *bg)
{
    cairo_content_t imgt = cairo_surface_get_content(image);
    cairo_content_t bgt = cairo_surface_get_content(bg);
    cairo_surface_t *out = NULL;

    if (bgt == CAIRO_CONTENT_ALPHA && imgt == CAIRO_CONTENT_ALPHA) {
        out = ink_cairo_surface_create_identical(bg);
    } else {
        out = ink_cairo_surface_create_same_size(bg, CAIRO_CONTENT_COLOR_ALPHA);
    }

    return out;
}

void
ink_cairo_surface_blit(cairo_surface_t *src, cairo_surface_t *dest)
{
    if (cairo_surface_get_type(src) == CAIRO_SURFACE_TYPE_IMAGE &&
        cairo_surface_get_type(dest) == CAIRO_SURFACE_TYPE_IMAGE &&
        cairo_image_surface_get_format(src) == cairo_image_surface_get_format(dest) &&
        cairo_image_surface_get_height(src) == cairo_image_surface_get_height(dest) &&
        cairo_image_surface_get_width(src) == cairo_image_surface_get_width(dest) &&
        cairo_image_surface_get_stride(src) == cairo_image_surface_get_stride(dest))
    {
        // use memory copy instead of using a Cairo context
        cairo_surface_flush(src);
        int stride = cairo_image_surface_get_stride(src);
        int h = cairo_image_surface_get_height(src);
        memcpy(cairo_image_surface_get_data(dest), cairo_image_surface_get_data(src), stride * h);
        cairo_surface_mark_dirty(dest);
    } else {
        // generic implementation
        cairo_t *ct = cairo_create(dest);
        cairo_set_source_surface(ct, src, 0, 0);
        cairo_set_operator(ct, CAIRO_OPERATOR_SOURCE);
        cairo_paint(ct);
        cairo_destroy(ct);
    }
}

int
ink_cairo_surface_get_width(cairo_surface_t *surface)
{
    // For now only image surface is handled.
    // Later add others, e.g. cairo-gl
    assert(cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE);
    return cairo_image_surface_get_width(surface);
}
int
ink_cairo_surface_get_height(cairo_surface_t *surface)
{
    assert(cairo_surface_get_type(surface) == CAIRO_SURFACE_TYPE_IMAGE);
    return cairo_image_surface_get_height(surface);
}

static int ink_cairo_surface_average_color_internal(cairo_surface_t *surface, double &rf, double &gf, double &bf, double &af)
{
    rf = gf = bf = af = 0.0;
    cairo_surface_flush(surface);
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);

    /* TODO convert this to OpenMP somehow */
    for (int y = 0; y < height; ++y, data += stride) {
        for (int x = 0; x < width; ++x) {
            guint32 px = *reinterpret_cast<guint32*>(data + 4*x);
            EXTRACT_ARGB32(px, a,r,g,b)
            rf += r / 255.0;
            gf += g / 255.0;
            bf += b / 255.0;
            af += a / 255.0;
        }
    }
    return width * height;
}

guint32 ink_cairo_surface_average_color(cairo_surface_t *surface)
{
    double rf,gf,bf,af;
    ink_cairo_surface_average_color_premul(surface, rf,gf,bf,af);
    guint32 r = round(rf * 255);
    guint32 g = round(gf * 255);
    guint32 b = round(bf * 255);
    guint32 a = round(af * 255);
    ASSEMBLE_ARGB32(px, a,r,g,b);
    return px;
}

void ink_cairo_surface_average_color(cairo_surface_t *surface, double &r, double &g, double &b, double &a)
{
    int count = ink_cairo_surface_average_color_internal(surface, r,g,b,a);

    r /= a;
    g /= a;
    b /= a;
    a /= count;

    r = CLAMP(r, 0.0, 1.0);
    g = CLAMP(g, 0.0, 1.0);
    b = CLAMP(b, 0.0, 1.0);
    a = CLAMP(a, 0.0, 1.0);
}

void ink_cairo_surface_average_color_premul(cairo_surface_t *surface, double &r, double &g, double &b, double &a)
{
    int count = ink_cairo_surface_average_color_internal(surface, r,g,b,a);

    r /= count;
    g /= count;
    b /= count;
    a /= count;

    r = CLAMP(r, 0.0, 1.0);
    g = CLAMP(g, 0.0, 1.0);
    b = CLAMP(b, 0.0, 1.0);
    a = CLAMP(a, 0.0, 1.0);
}

cairo_pattern_t *
ink_cairo_pattern_create_checkerboard()
{
    int const w = 6;
    int const h = 6;

    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2*w, 2*h);

    cairo_t *ct = cairo_create(s);
    cairo_set_operator(ct, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(ct, 0.75, 0.75, 0.75);
    cairo_paint(ct);
    cairo_set_source_rgb(ct, 0.5, 0.5, 0.5);
    cairo_rectangle(ct, 0, 0, w, h);
    cairo_rectangle(ct, w, h, w, h);
    cairo_fill(ct);
    cairo_destroy(ct);

    cairo_pattern_t *p = cairo_pattern_create_for_surface(s);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
    cairo_pattern_set_filter(p, CAIRO_FILTER_NEAREST);

    cairo_surface_destroy(s);
    return p;
}

/* The following two functions use "from" instead of "to", because when you write:
   val1 = argb32_from_pixbuf(val1);
   the name of the format is closer to the value in that format. */

guint32 argb32_from_pixbuf(guint32 c)
{
    guint32 o = 0;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    guint32 a = (c & 0xff000000) >> 24;
#else
    guint32 a = (c & 0x000000ff);
#endif
    if (a != 0) {
        // extract color components
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
        guint32 r = (c & 0x000000ff);
        guint32 g = (c & 0x0000ff00) >> 8;
        guint32 b = (c & 0x00ff0000) >> 16;
#else
        guint32 r = (c & 0xff000000) >> 24;
        guint32 g = (c & 0x00ff0000) >> 16;
        guint32 b = (c & 0x0000ff00) >> 8;
#endif
        // premultiply
        r = premul_alpha(r, a);
        b = premul_alpha(b, a);
        g = premul_alpha(g, a);
        // combine into output
        o = (a << 24) | (r << 16) | (g << 8) | (b);
    }
    return o;
}

guint32 pixbuf_from_argb32(guint32 c)
{
    guint32 a = (c & 0xff000000) >> 24;
    if (a == 0) return 0;

    // extract color components
    guint32 r = (c & 0x00ff0000) >> 16;
    guint32 g = (c & 0x0000ff00) >> 8;
    guint32 b = (c & 0x000000ff);
    // unpremultiply; adding a/2 gives correct rounding
    // (taken from Cairo sources)
    r = (r * 255 + a/2) / a;
    b = (b * 255 + a/2) / a;
    g = (g * 255 + a/2) / a;
    // combine into output
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    guint32 o = (r) | (g << 8) | (b << 16) | (a << 24);
#else
    guint32 o = (r << 24) | (g << 16) | (b << 8) | (a);
#endif
    return o;
}

/**
 * Convert pixel data from GdkPixbuf format to ARGB.
 * This will convert pixel data from GdkPixbuf format to Cairo's native pixel format.
 * This involves premultiplying alpha and shuffling around the channels.
 * Pixbuf data must have an alpha channel, otherwise the results are undefined
 * (usually a segfault).
 */
void
convert_pixels_pixbuf_to_argb32(guchar *data, int w, int h, int stride)
{
    for (int i = 0; i < h; ++i) {
        guint32 *px = reinterpret_cast<guint32*>(data + i*stride);
        for (int j = 0; j < w; ++j) {
            *px = argb32_from_pixbuf(*px);
            ++px;
        }
    }
}

/**
 * Convert pixel data from ARGB to GdkPixbuf format.
 * This will convert pixel data from GdkPixbuf format to Cairo's native pixel format.
 * This involves premultiplying alpha and shuffling around the channels.
 */
void
convert_pixels_argb32_to_pixbuf(guchar *data, int w, int h, int stride)
{
    for (int i = 0; i < h; ++i) {
        guint32 *px = reinterpret_cast<guint32*>(data + i*stride);
        for (int j = 0; j < w; ++j) {
            *px = pixbuf_from_argb32(*px);
            ++px;
        }
    }
}

/**
 * Converts GdkPixbuf's data to premultiplied ARGB.
 * This function will convert a GdkPixbuf in place into Cairo's native pixel format.
 * Note that this is a hack intended to save memory. When the pixbuf is in Cairo's format,
 * using it with GTK will result in corrupted drawings.
 */
void
convert_pixbuf_normal_to_argb32(GdkPixbuf *pb)
{
    convert_pixels_pixbuf_to_argb32(
        gdk_pixbuf_get_pixels(pb),
        gdk_pixbuf_get_width(pb),
        gdk_pixbuf_get_height(pb),
        gdk_pixbuf_get_rowstride(pb));
}

/**
 * Converts GdkPixbuf's data back to its native format.
 * Once this is done, the pixbuf can be used with GTK again.
 */
void
convert_pixbuf_argb32_to_normal(GdkPixbuf *pb)
{
    convert_pixels_argb32_to_pixbuf(
        gdk_pixbuf_get_pixels(pb),
        gdk_pixbuf_get_width(pb),
        gdk_pixbuf_get_height(pb),
        gdk_pixbuf_get_rowstride(pb));
}

guint32 argb32_from_rgba(guint32 in)
{
    guint32 r, g, b, a;
    a = (in & 0x000000ff);
    r = premul_alpha((in & 0xff000000) >> 24, a);
    g = premul_alpha((in & 0x00ff0000) >> 16, a);
    b = premul_alpha((in & 0x0000ff00) >> 8,  a);
    ASSEMBLE_ARGB32(px, a, r, g, b)
    return px;
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=99 :
