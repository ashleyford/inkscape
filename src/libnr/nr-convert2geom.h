#ifndef INKSCAPE_LIBNR_CONVERT2GEOM_H
#define INKSCAPE_LIBNR_CONVERT2GEOM_H

/*
 * Converts between NR and 2Geom types.
 *
* Copyright (C) Johan Engelen 2008 <j.b.c.engelen@utwente.nl>
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */

#include <libnr/nr-rect.h>
#include <libnr/nr-point.h>
#include <2geom/matrix.h>
#include <2geom/d2.h>
#include <2geom/transforms.h>
#include <2geom/point.h>

inline Geom::Point to_2geom(NR::Point const & _pt) {
    return Geom::Point(_pt[0], _pt[1]);
}
inline NR::Point from_2geom(Geom::Point const & _pt) {
    return NR::Point(_pt[0], _pt[1]);
}

inline Geom::Rect to_2geom(NR::Rect const & rect) {
    Geom::Rect rect2geom(to_2geom(rect.min()), to_2geom(rect.max()));
    return rect2geom;
}
inline NR::Rect from_2geom(Geom::Rect const & rect2geom) {
    NR::Rect rect(rect2geom.min(), rect2geom.max());
    return rect;
}
inline Geom::OptRect to_2geom(boost::optional<NR::Rect> const & rect) {
    Geom::OptRect rect2geom;
    if (!rect) {
        return rect2geom;
    }
    rect2geom = to_2geom(*rect);
    return rect2geom;
}

#endif

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
