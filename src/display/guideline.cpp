#define __SP_GUIDELINE_C__

/*
 * Horizontal/vertical but can also be angled line
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   Johan Engelen
 *   Maximilian Albert <maximilian.albert@gmail.com>
 *
 * Copyright (C) 2000-2002 Lauris Kaplinski
 * Copyright (C) 2007 Johan Engelen
 * Copyright (C) 2009 Maximilian Albert
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */

#include <2geom/transforms.h>
#include "display-forward.h"
#include "sp-canvas-util.h"
#include "sp-ctrlpoint.h"
#include "guideline.h"
#include "display/cairo-utils.h"

static void sp_guideline_class_init(SPGuideLineClass *c);
static void sp_guideline_init(SPGuideLine *guideline);
static void sp_guideline_destroy(GtkObject *object);

static void sp_guideline_update(SPCanvasItem *item, Geom::Matrix const &affine, unsigned int flags);
static void sp_guideline_render(SPCanvasItem *item, SPCanvasBuf *buf);

static double sp_guideline_point(SPCanvasItem *item, Geom::Point p, SPCanvasItem **actual_item);

static void sp_guideline_drawline (SPCanvasBuf *buf, gint x0, gint y0, gint x1, gint y1, guint32 rgba);

static SPCanvasItemClass *parent_class;

GType sp_guideline_get_type()
{
    static GType guideline_type = 0;

    if (!guideline_type) {
        static GTypeInfo const guideline_info = {
            sizeof (SPGuideLineClass),
            NULL, NULL,
            (GClassInitFunc) sp_guideline_class_init,
            NULL, NULL,
            sizeof (SPGuideLine),
            16,
            (GInstanceInitFunc) sp_guideline_init,
            NULL,
        };

        guideline_type = g_type_register_static(SP_TYPE_CANVAS_ITEM, "SPGuideLine", &guideline_info, (GTypeFlags) 0);
    }

    return guideline_type;
}

static void sp_guideline_class_init(SPGuideLineClass *c)
{
    parent_class = (SPCanvasItemClass*) g_type_class_peek_parent(c);

    GtkObjectClass *object_class = (GtkObjectClass *) c;
    object_class->destroy = sp_guideline_destroy;

    SPCanvasItemClass *item_class = (SPCanvasItemClass *) c;
    item_class->update = sp_guideline_update;
    item_class->render = sp_guideline_render;
    item_class->point = sp_guideline_point;
}

static void sp_guideline_init(SPGuideLine *gl)
{
    gl->rgba = 0x0000ff7f;

    gl->normal_to_line = Geom::Point(0,1);
    gl->angle = 3.14159265358979323846/2;
    gl->point_on_line = Geom::Point(0,0);
    gl->sensitive = 0;

    gl->origin = NULL;
}

static void sp_guideline_destroy(GtkObject *object)
{
    g_return_if_fail (object != NULL);
    g_return_if_fail (SP_IS_GUIDELINE (object));
    //g_return_if_fail (SP_GUIDELINE(object)->origin != NULL);
    //g_return_if_fail (SP_IS_CTRLPOINT(SP_GUIDELINE(object)->origin));
    
    if (SP_GUIDELINE(object)->origin != NULL && SP_IS_CTRLPOINT(SP_GUIDELINE(object)->origin)) {
        gtk_object_destroy(GTK_OBJECT(SP_GUIDELINE(object)->origin));
    } else {
        // FIXME: This branch shouldn't be reached (although it seems to be harmless).
        //g_error("Why can it be that gl->origin is not a valid SPCtrlPoint?\n");
    }

    GTK_OBJECT_CLASS(parent_class)->destroy(object);
}

static void sp_guideline_render(SPCanvasItem *item, SPCanvasBuf *buf)
{
    SPGuideLine const *gl = SP_GUIDELINE (item);

    cairo_save(buf->ct);
    cairo_translate(buf->ct, -buf->rect.x0, -buf->rect.y0);
    ink_cairo_set_source_rgba32(buf->ct, gl->rgba);
    cairo_set_line_width(buf->ct, 1);
    cairo_set_line_cap(buf->ct, CAIRO_LINE_CAP_SQUARE);

    if (gl->is_vertical()) {
        int position = (int) Inkscape::round(gl->point_on_line[Geom::X]);
        cairo_move_to(buf->ct, position + 0.5, buf->rect.y0 + 0.5);
        cairo_line_to(buf->ct, position + 0.5, buf->rect.y1 - 0.5);
        cairo_stroke(buf->ct);
    } else if (gl->is_horizontal()) {
        int position = (int) Inkscape::round(gl->point_on_line[Geom::Y]);
        cairo_move_to(buf->ct, buf->rect.x0 + 0.5, position + 0.5);
        cairo_line_to(buf->ct, buf->rect.x1 - 0.5, position + 0.5);
        cairo_stroke(buf->ct);
    } else {
        // render angled line, once intersection has been detected, draw from there.
        Geom::Point parallel_to_line( gl->normal_to_line[Geom::Y],
                                      /*should be minus, but inverted y axis*/ gl->normal_to_line[Geom::X]);

        //try to intersect with left vertical of rect
        double y_intersect_left = (buf->rect.x0 - gl->point_on_line[Geom::X]) * parallel_to_line[Geom::Y] / parallel_to_line[Geom::X] + gl->point_on_line[Geom::Y];
        if ( (y_intersect_left >= buf->rect.y0) && (y_intersect_left <= buf->rect.y1) ) {
            // intersects with left vertical!
            double y_intersect_right = (buf->rect.x1 - gl->point_on_line[Geom::X]) * parallel_to_line[Geom::Y] / parallel_to_line[Geom::X] + gl->point_on_line[Geom::Y];
            sp_guideline_drawline (buf, buf->rect.x0, static_cast<gint>(round(y_intersect_left)), buf->rect.x1, static_cast<gint>(round(y_intersect_right)), gl->rgba);
            goto end;
        }

        //try to intersect with right vertical of rect
        double y_intersect_right = (buf->rect.x1 - gl->point_on_line[Geom::X]) * parallel_to_line[Geom::Y] / parallel_to_line[Geom::X] + gl->point_on_line[Geom::Y];
        if ( (y_intersect_right >= buf->rect.y0) && (y_intersect_right <= buf->rect.y1) ) {
            // intersects with right vertical!
            sp_guideline_drawline (buf, buf->rect.x1, static_cast<gint>(round(y_intersect_right)), buf->rect.x0, static_cast<gint>(round(y_intersect_left)), gl->rgba);
            goto end;
        }

        //try to intersect with top horizontal of rect
        double x_intersect_top = (buf->rect.y0 - gl->point_on_line[Geom::Y]) * parallel_to_line[Geom::X] / parallel_to_line[Geom::Y] + gl->point_on_line[Geom::X];
        if ( (x_intersect_top >= buf->rect.x0) && (x_intersect_top <= buf->rect.x1) ) {
            // intersects with top horizontal!
            double x_intersect_bottom = (buf->rect.y1 - gl->point_on_line[Geom::Y]) * parallel_to_line[Geom::X] / parallel_to_line[Geom::Y] + gl->point_on_line[Geom::X];
            sp_guideline_drawline (buf, static_cast<gint>(round(x_intersect_top)), buf->rect.y0, static_cast<gint>(round(x_intersect_bottom)), buf->rect.y1, gl->rgba);
            goto end;
        }

        //try to intersect with bottom horizontal of rect
        double x_intersect_bottom = (buf->rect.y1 - gl->point_on_line[Geom::Y]) * parallel_to_line[Geom::X] / parallel_to_line[Geom::Y] + gl->point_on_line[Geom::X];
        if ( (x_intersect_top >= buf->rect.x0) && (x_intersect_top <= buf->rect.x1) ) {
            // intersects with bottom horizontal!
            sp_guideline_drawline (buf, static_cast<gint>(round(x_intersect_bottom)), buf->rect.y1, static_cast<gint>(round(x_intersect_top)), buf->rect.y0, gl->rgba);
            goto end;
        }
    }
    end:
    cairo_restore(buf->ct);
}

static void sp_guideline_update(SPCanvasItem *item, Geom::Matrix const &affine, unsigned int flags)
{
    SPGuideLine *gl = SP_GUIDELINE(item);

    if (((SPCanvasItemClass *) parent_class)->update) {
        ((SPCanvasItemClass *) parent_class)->update(item, affine, flags);
    }

    gl->point_on_line[Geom::X] = affine[4];
    gl->point_on_line[Geom::Y] = affine[5];

    sp_ctrlpoint_set_coords(gl->origin, gl->point_on_line * affine.inverse());
    sp_canvas_item_request_update(SP_CANVAS_ITEM (gl->origin));

    if (gl->is_horizontal()) {
        sp_canvas_update_bbox (item, -1000000, (int) Inkscape::round(gl->point_on_line[Geom::Y]), 1000000, (int) Inkscape::round(gl->point_on_line[Geom::Y] + 1));
    } else if (gl->is_vertical()) {
        sp_canvas_update_bbox (item, (int) Inkscape::round(gl->point_on_line[Geom::X]), -1000000, (int) Inkscape::round(gl->point_on_line[Geom::X] + 1), 1000000);
    } else {
        sp_canvas_update_bbox (item, -1000000, -1000000, 1000000, 1000000);
    }
}

// Returns 0.0 if point is on the guideline
static double sp_guideline_point(SPCanvasItem *item, Geom::Point p, SPCanvasItem **actual_item)
{
    SPGuideLine *gl = SP_GUIDELINE (item);

    if (!gl->sensitive) {
        return Geom::infinity();
    }

    *actual_item = item;

    Geom::Point vec(gl->normal_to_line[Geom::X], - gl->normal_to_line[Geom::Y]);
    double distance = Geom::dot((p - gl->point_on_line), vec);
    return MAX(fabs(distance)-1, 0);
}

SPCanvasItem *sp_guideline_new(SPCanvasGroup *parent, Geom::Point point_on_line, Geom::Point normal)
{
    SPCanvasItem *item = sp_canvas_item_new(parent, SP_TYPE_GUIDELINE, NULL);
    SPCanvasItem *origin = sp_canvas_item_new(parent, SP_TYPE_CTRLPOINT, NULL);

    SPGuideLine *gl = SP_GUIDELINE(item);
    SPCtrlPoint *cp = SP_CTRLPOINT(origin);
    gl->origin = cp;

    normal.normalize();
    gl->normal_to_line = normal;
    gl->angle = tan( -gl->normal_to_line[Geom::X] / gl->normal_to_line[Geom::Y]);
    sp_guideline_set_position(gl, point_on_line);

    sp_ctrlpoint_set_coords(cp, point_on_line);

    return item;
}

void sp_guideline_set_position(SPGuideLine *gl, Geom::Point point_on_line)
{
    sp_canvas_item_affine_absolute(SP_CANVAS_ITEM (gl), Geom::Matrix(Geom::Translate(point_on_line)));
    sp_canvas_item_affine_absolute(SP_CANVAS_ITEM (gl->origin), Geom::Matrix(Geom::Translate(point_on_line)));
}

void sp_guideline_set_normal(SPGuideLine *gl, Geom::Point normal_to_line)
{
    gl->normal_to_line = normal_to_line;
    gl->angle = tan( -normal_to_line[Geom::X] / normal_to_line[Geom::Y]);

    sp_canvas_item_request_update(SP_CANVAS_ITEM (gl));
}

void sp_guideline_set_color(SPGuideLine *gl, unsigned int rgba)
{
    gl->rgba = rgba;
    sp_ctrlpoint_set_color(gl->origin, rgba);

    sp_canvas_item_request_update(SP_CANVAS_ITEM(gl));
}

void sp_guideline_set_sensitive(SPGuideLine *gl, int sensitive)
{
    gl->sensitive = sensitive;
}

void sp_guideline_delete(SPGuideLine *gl)
{
    //gtk_object_destroy(GTK_OBJECT(gl->origin));
    gtk_object_destroy(GTK_OBJECT(gl));
}

static void
sp_guideline_drawline (SPCanvasBuf *buf, gint x0, gint y0, gint x1, gint y1, guint32 /*rgba*/)
{
    cairo_move_to(buf->ct, x0 + 0.5, y0 + 0.5);
    cairo_line_to(buf->ct, x1 + 0.5, y1 + 0.5);
    cairo_stroke(buf->ct);
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
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
