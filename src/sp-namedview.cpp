/*
 * <sodipodi:namedview> implementation
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   bulia byak <buliabyak@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *
 * Copyright (C) 2006      Johan Engelen <johan@shouraizou.nl>
 * Copyright (C) 1999-2008 Authors
 * Copyright (C) 2000-2001 Ximian, Inc.
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */

#include "config.h"
#include <cstring>
#include <string>
#include "event-log.h"
#include <2geom/transforms.h>

#include "display/canvas-grid.h"
#include "display/guideline.h"
#include "helper/units.h"
#include "svg/svg-color.h"
#include "xml/repr.h"
#include "attributes.h"
#include "document.h"
#include "document-undo.h"
#include "desktop-events.h"
#include "desktop-handles.h"
#include "sp-guide.h"
#include "sp-item-group.h"
#include "sp-namedview.h"
#include "preferences.h"
#include "desktop.h"
#include "conn-avoid-ref.h" // for defaultConnSpacing.
#include "sp-root.h"
#include <gtkmm/window.h>

using Inkscape::DocumentUndo;

#define DEFAULTGRIDCOLOR 0x3f3fff25
#define DEFAULTGRIDEMPCOLOR 0x3f3fff60
#define DEFAULTGRIDEMPSPACING 5
#define DEFAULTGUIDECOLOR 0x0000ff7f
#define DEFAULTGUIDEHICOLOR 0xff00007f
#define DEFAULTBORDERCOLOR 0x000000ff
#define DEFAULTPAGECOLOR 0xffffff00

static void sp_namedview_class_init(SPNamedViewClass *klass);
static void sp_namedview_init(SPNamedView *namedview);

static void sp_namedview_build(SPObject *object, SPDocument *document, Inkscape::XML::Node *repr);
static void sp_namedview_release(SPObject *object);
static void sp_namedview_set(SPObject *object, unsigned int key, const gchar *value);
static void sp_namedview_child_added(SPObject *object, Inkscape::XML::Node *child, Inkscape::XML::Node *ref);
static void sp_namedview_remove_child(SPObject *object, Inkscape::XML::Node *child);
static Inkscape::XML::Node *sp_namedview_write(SPObject *object, Inkscape::XML::Document *doc, Inkscape::XML::Node *repr, guint flags);

static void sp_namedview_setup_guides(SPNamedView * nv);
static void sp_namedview_show_single_guide(SPGuide* guide, bool show);

static gboolean sp_str_to_bool(const gchar *str);
static gboolean sp_nv_read_opacity(const gchar *str, guint32 *color);

static SPObjectGroupClass * parent_class;

GType
sp_namedview_get_type()
{
    static GType namedview_type = 0;
    if (!namedview_type) {
        GTypeInfo namedview_info = {
            sizeof(SPNamedViewClass),
            NULL,       /* base_init */
            NULL,       /* base_finalize */
            (GClassInitFunc) sp_namedview_class_init,
            NULL,       /* class_finalize */
            NULL,       /* class_data */
            sizeof(SPNamedView),
            16, /* n_preallocs */
            (GInstanceInitFunc) sp_namedview_init,
            NULL,       /* value_table */
        };
        namedview_type = g_type_register_static(SP_TYPE_OBJECTGROUP, "SPNamedView", &namedview_info, (GTypeFlags)0);
    }
    return namedview_type;
}

static void sp_namedview_class_init(SPNamedViewClass * klass)
{
    SPObjectClass *sp_object_class = reinterpret_cast<SPObjectClass *>(klass);

    parent_class = reinterpret_cast<SPObjectGroupClass *>(g_type_class_ref(SP_TYPE_OBJECTGROUP));

    sp_object_class->build = sp_namedview_build;
    sp_object_class->release = sp_namedview_release;
    sp_object_class->set = sp_namedview_set;
    sp_object_class->child_added = sp_namedview_child_added;
    sp_object_class->remove_child = sp_namedview_remove_child;
    sp_object_class->write = sp_namedview_write;
}

static void sp_namedview_init(SPNamedView *nv)
{
    nv->editable = TRUE;
    nv->showguides = TRUE;
    nv->grids_visible = false;
    nv->showborder = TRUE;
    nv->showpageshadow = TRUE;

    nv->guides = NULL;
    nv->viewcount = 0;
    nv->grids = NULL;

    nv->default_layer_id = 0;

    nv->connector_spacing = defaultConnSpacing;

    new (&nv->snap_manager) SnapManager(nv);
}

static void sp_namedview_generate_old_grid(SPNamedView * /*nv*/, SPDocument *document, Inkscape::XML::Node *repr) {
    bool old_grid_settings_present = false;

    // set old settings
    const char* gridspacingx    = "1px";
    const char* gridspacingy    = "1px";
    const char* gridoriginy     = "0px";
    const char* gridoriginx     = "0px";
    const char* gridempspacing  = "5";
    const char* gridcolor       = "#3f3fff";
    const char* gridempcolor    = "#3f3fff";
    const char* gridopacity     = "0.15";
    const char* gridempopacity  = "0.38";

    const char* value = NULL;
    if ((value = repr->attribute("gridoriginx"))) {
        gridoriginx = value;
        old_grid_settings_present = true;
    }
    if ((value = repr->attribute("gridoriginy"))) {
        gridoriginy = value;
        old_grid_settings_present = true;
    }
    if ((value = repr->attribute("gridspacingx"))) {
        gridspacingx = value;
        old_grid_settings_present = true;
    }
    if ((value = repr->attribute("gridspacingy"))) {
        gridspacingy = value;
        old_grid_settings_present = true;
    }
    if ((value = repr->attribute("gridcolor"))) {
        gridcolor = value;
        old_grid_settings_present = true;
    }
    if ((value = repr->attribute("gridempcolor"))) {
        gridempcolor = value;
        old_grid_settings_present = true;
    }
    if ((value = repr->attribute("gridempspacing"))) {
        gridempspacing = value;
        old_grid_settings_present = true;
    }
    if ((value = repr->attribute("gridopacity"))) {
        gridopacity = value;
        old_grid_settings_present = true;
    }
    if ((value = repr->attribute("gridempopacity"))) {
        gridempopacity = value;
        old_grid_settings_present = true;
    }

    if (old_grid_settings_present) {
        // generate new xy grid with the correct settings
        // first create the child xml node, then hook it to repr. This order is important, to not set off listeners to repr before the new node is complete.

        Inkscape::XML::Document *xml_doc = document->getReprDoc();
        Inkscape::XML::Node *newnode = xml_doc->createElement("inkscape:grid");
        newnode->setAttribute("id", "GridFromPre046Settings");
        newnode->setAttribute("type", Inkscape::CanvasGrid::getSVGName(Inkscape::GRID_RECTANGULAR));
        newnode->setAttribute("originx", gridoriginx);
        newnode->setAttribute("originy", gridoriginy);
        newnode->setAttribute("spacingx", gridspacingx);
        newnode->setAttribute("spacingy", gridspacingy);
        newnode->setAttribute("color", gridcolor);
        newnode->setAttribute("empcolor", gridempcolor);
        newnode->setAttribute("opacity", gridopacity);
        newnode->setAttribute("empopacity", gridempopacity);
        newnode->setAttribute("empspacing", gridempspacing);

        repr->appendChild(newnode);
        Inkscape::GC::release(newnode);

        // remove all old settings
        repr->setAttribute("gridoriginx", NULL);
        repr->setAttribute("gridoriginy", NULL);
        repr->setAttribute("gridspacingx", NULL);
        repr->setAttribute("gridspacingy", NULL);
        repr->setAttribute("gridcolor", NULL);
        repr->setAttribute("gridempcolor", NULL);
        repr->setAttribute("gridopacity", NULL);
        repr->setAttribute("gridempopacity", NULL);
        repr->setAttribute("gridempspacing", NULL);

//        SPDocumentUndo::done(doc, SP_VERB_DIALOG_NAMEDVIEW, _("Create new grid from pre0.46 grid settings"));
    }
}

static void sp_namedview_build(SPObject *object, SPDocument *document, Inkscape::XML::Node *repr)
{
    SPNamedView *nv = (SPNamedView *) object;
    SPObjectGroup *og = (SPObjectGroup *) object;

    if (((SPObjectClass *) (parent_class))->build) {
        (* ((SPObjectClass *) (parent_class))->build)(object, document, repr);
    }

    object->readAttr( "inkscape:document-units" );
    object->readAttr( "units" );
    object->readAttr( "viewonly" );
    object->readAttr( "showguides" );
    object->readAttr( "showgrid" );
    object->readAttr( "gridtolerance" );
    object->readAttr( "guidetolerance" );
    object->readAttr( "objecttolerance" );
    object->readAttr( "guidecolor" );
    object->readAttr( "guideopacity" );
    object->readAttr( "guidehicolor" );
    object->readAttr( "guidehiopacity" );
    object->readAttr( "showborder" );
    object->readAttr( "inkscape:showpageshadow" );
    object->readAttr( "borderlayer" );
    object->readAttr( "bordercolor" );
    object->readAttr( "borderopacity" );
    object->readAttr( "pagecolor" );
    object->readAttr( "inkscape:pageopacity" );
    object->readAttr( "inkscape:pageshadow" );
    object->readAttr( "inkscape:zoom" );
    object->readAttr( "inkscape:cx" );
    object->readAttr( "inkscape:cy" );
    object->readAttr( "inkscape:window-width" );
    object->readAttr( "inkscape:window-height" );
    object->readAttr( "inkscape:window-x" );
    object->readAttr( "inkscape:window-y" );
    object->readAttr( "inkscape:window-maximized" );
    object->readAttr( "inkscape:snap-global" );
    object->readAttr( "inkscape:snap-bbox" );
    object->readAttr( "inkscape:snap-nodes" );
    object->readAttr( "inkscape:snap-others" );
    object->readAttr( "inkscape:snap-from-guide" );
    object->readAttr( "inkscape:snap-center" );
    object->readAttr( "inkscape:snap-smooth-nodes" );
    object->readAttr( "inkscape:snap-midpoints" );
    object->readAttr( "inkscape:snap-object-midpoints" );
    object->readAttr( "inkscape:snap-text-baseline" );
    object->readAttr( "inkscape:snap-bbox-edge-midpoints" );
    object->readAttr( "inkscape:snap-bbox-midpoints" );
    object->readAttr( "inkscape:snap-to-guides" );
    object->readAttr( "inkscape:snap-grids" );
    object->readAttr( "inkscape:snap-intersection-paths" );
    object->readAttr( "inkscape:object-paths" );
    object->readAttr( "inkscape:snap-perpendicular" );
    object->readAttr( "inkscape:snap-tangential" );
    object->readAttr( "inkscape:snap-path-clip" );
    object->readAttr( "inkscape:snap-path-mask" );
    object->readAttr( "inkscape:object-nodes" );
    object->readAttr( "inkscape:bbox-paths" );
    object->readAttr( "inkscape:bbox-nodes" );
    object->readAttr( "inkscape:snap-page" );
    object->readAttr( "inkscape:current-layer" );
    object->readAttr( "inkscape:connector-spacing" );

    /* Construct guideline list */
    for (SPObject *o = og->firstChild() ; o; o = o->getNext() ) {
        if (SP_IS_GUIDE(o)) {
            SPGuide * g = SP_GUIDE(o);
            nv->guides = g_slist_prepend(nv->guides, g);
            g_object_set(G_OBJECT(g), "color", nv->guidecolor, "hicolor", nv->guidehicolor, NULL);
        }
    }

    // backwards compatibility with grid settings (pre 0.46)
    sp_namedview_generate_old_grid(nv, document, repr);
}

static void sp_namedview_release(SPObject *object)
{
    SPNamedView *namedview = (SPNamedView *) object;

    if (namedview->guides) {
        g_slist_free(namedview->guides);
        namedview->guides = NULL;
    }

    // delete grids:
    while ( namedview->grids ) {
        Inkscape::CanvasGrid *gr = (Inkscape::CanvasGrid *)namedview->grids->data; // get first entry
        delete gr;
        namedview->grids = g_slist_remove_link(namedview->grids, namedview->grids); // deletes first entry
    }

    if (((SPObjectClass *) parent_class)->release) {
        ((SPObjectClass *) parent_class)->release(object);
    }

    namedview->snap_manager.~SnapManager();
}

static void sp_namedview_set(SPObject *object, unsigned int key, const gchar *value)
{
    SPNamedView *nv = SP_NAMEDVIEW(object);

    switch (key) {
    case SP_ATTR_VIEWONLY:
            nv->editable = (!value);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_SHOWGUIDES:
            if (!value) { // show guides if not specified, for backwards compatibility
                nv->showguides = TRUE;
            } else {
                nv->showguides = sp_str_to_bool(value);
            }
            sp_namedview_setup_guides(nv);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_SHOWGRIDS:
            if (!value) { // don't show grids if not specified, for backwards compatibility
                nv->grids_visible = false;
            } else {
                nv->grids_visible = sp_str_to_bool(value);
            }
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_GRIDTOLERANCE:
            nv->snap_manager.snapprefs.setGridTolerance(value ? g_ascii_strtod(value, NULL) : 10000);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_GUIDETOLERANCE:
            nv->snap_manager.snapprefs.setGuideTolerance(value ? g_ascii_strtod(value, NULL) : 20);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_OBJECTTOLERANCE:
            nv->snap_manager.snapprefs.setObjectTolerance(value ? g_ascii_strtod(value, NULL) : 20);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_GUIDECOLOR:
            nv->guidecolor = (nv->guidecolor & 0xff) | (DEFAULTGUIDECOLOR & 0xffffff00);
            if (value) {
                nv->guidecolor = (nv->guidecolor & 0xff) | sp_svg_read_color(value, nv->guidecolor);
            }
            for (GSList *l = nv->guides; l != NULL; l = l->next) {
                g_object_set(G_OBJECT(l->data), "color", nv->guidecolor, NULL);
            }
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_GUIDEOPACITY:
            nv->guidecolor = (nv->guidecolor & 0xffffff00) | (DEFAULTGUIDECOLOR & 0xff);
            sp_nv_read_opacity(value, &nv->guidecolor);
            for (GSList *l = nv->guides; l != NULL; l = l->next) {
                g_object_set(G_OBJECT(l->data), "color", nv->guidecolor, NULL);
            }
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_GUIDEHICOLOR:
            nv->guidehicolor = (nv->guidehicolor & 0xff) | (DEFAULTGUIDEHICOLOR & 0xffffff00);
            if (value) {
                nv->guidehicolor = (nv->guidehicolor & 0xff) | sp_svg_read_color(value, nv->guidehicolor);
            }
            for (GSList *l = nv->guides; l != NULL; l = l->next) {
                g_object_set(G_OBJECT(l->data), "hicolor", nv->guidehicolor, NULL);
            }
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_GUIDEHIOPACITY:
            nv->guidehicolor = (nv->guidehicolor & 0xffffff00) | (DEFAULTGUIDEHICOLOR & 0xff);
            sp_nv_read_opacity(value, &nv->guidehicolor);
            for (GSList *l = nv->guides; l != NULL; l = l->next) {
                g_object_set(G_OBJECT(l->data), "hicolor", nv->guidehicolor, NULL);
            }
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_SHOWBORDER:
            nv->showborder = (value) ? sp_str_to_bool (value) : TRUE;
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_BORDERLAYER:
            nv->borderlayer = SP_BORDER_LAYER_BOTTOM;
            if (value && !strcasecmp(value, "true")) nv->borderlayer = SP_BORDER_LAYER_TOP;
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_BORDERCOLOR:
            nv->bordercolor = (nv->bordercolor & 0xff) | (DEFAULTBORDERCOLOR & 0xffffff00);
            if (value) {
                nv->bordercolor = (nv->bordercolor & 0xff) | sp_svg_read_color (value, nv->bordercolor);
            }
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_BORDEROPACITY:
            nv->bordercolor = (nv->bordercolor & 0xffffff00) | (DEFAULTBORDERCOLOR & 0xff);
            sp_nv_read_opacity(value, &nv->bordercolor);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_PAGECOLOR:
            nv->pagecolor = (nv->pagecolor & 0xff) | (DEFAULTPAGECOLOR & 0xffffff00);
            if (value) {
                nv->pagecolor = (nv->pagecolor & 0xff) | sp_svg_read_color(value, nv->pagecolor);
            }
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_PAGEOPACITY:
            nv->pagecolor = (nv->pagecolor & 0xffffff00) | (DEFAULTPAGECOLOR & 0xff);
            sp_nv_read_opacity(value, &nv->pagecolor);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_PAGESHADOW:
            nv->pageshadow = value? atoi(value) : 2; // 2 is the default
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_SHOWPAGESHADOW:
            nv->showpageshadow = (value) ? sp_str_to_bool(value) : TRUE;
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_ZOOM:
            nv->zoom = value ? g_ascii_strtod(value, NULL) : 0; // zero means not set
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_CX:
            nv->cx = value ? g_ascii_strtod(value, NULL) : HUGE_VAL; // HUGE_VAL means not set
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_CY:
            nv->cy = value ? g_ascii_strtod(value, NULL) : HUGE_VAL; // HUGE_VAL means not set
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_WINDOW_WIDTH:
            nv->window_width = value? atoi(value) : -1; // -1 means not set
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_WINDOW_HEIGHT:
            nv->window_height = value ? atoi(value) : -1; // -1 means not set
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_WINDOW_X:
            nv->window_x = value ? atoi(value) : 0;
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_WINDOW_Y:
            nv->window_y = value ? atoi(value) : 0;
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_WINDOW_MAXIMIZED:
            nv->window_maximized = value ? atoi(value) : 0;
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_GLOBAL:
            nv->snap_manager.snapprefs.setSnapEnabledGlobally(value ? sp_str_to_bool(value) : TRUE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_BBOX:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_BBOX_CATEGORY, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_NODE:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_NODE_CATEGORY, value ? sp_str_to_bool(value) : TRUE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_OTHERS:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_OTHERS_CATEGORY, value ? sp_str_to_bool(value) : TRUE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_ROTATION_CENTER:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_ROTATION_CENTER, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_GRID:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_GRID, value ? sp_str_to_bool(value) : TRUE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_GUIDE:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_GUIDE, value ? sp_str_to_bool(value) : TRUE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_NODE_SMOOTH:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_NODE_SMOOTH, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_LINE_MIDPOINT:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_LINE_MIDPOINT, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_OBJECT_MIDPOINT:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_OBJECT_MIDPOINT, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_TEXT_BASELINE:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_TEXT_BASELINE, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_BBOX_EDGE_MIDPOINT:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_BBOX_EDGE_MIDPOINT, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_BBOX_MIDPOINT:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_BBOX_MIDPOINT, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_PATH_INTERSECTION:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_PATH_INTERSECTION, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_PATH:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_PATH, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_PERP:
            nv->snap_manager.snapprefs.setSnapPerp(value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_TANG:
            nv->snap_manager.snapprefs.setSnapTang(value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_PATH_CLIP:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_PATH_CLIP, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_PATH_MASK:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_PATH_MASK, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_NODE_CUSP:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_NODE_CUSP, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_BBOX_EDGE:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_BBOX_EDGE, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_BBOX_CORNER:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_BBOX_CORNER, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_SNAP_PAGE_BORDER:
            nv->snap_manager.snapprefs.setTargetSnappable(Inkscape::SNAPTARGET_PAGE_BORDER, value ? sp_str_to_bool(value) : FALSE);
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_CURRENT_LAYER:
            nv->default_layer_id = value ? g_quark_from_string(value) : 0;
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_CONNECTOR_SPACING:
            nv->connector_spacing = value ? g_ascii_strtod(value, NULL) :
                    defaultConnSpacing;
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    case SP_ATTR_INKSCAPE_DOCUMENT_UNITS: {
            /* The default unit if the document doesn't override this: e.g. for files saved as
             * `plain SVG', or non-inkscape files, or files created by an inkscape 0.40 &
             * earlier.
             *
             * Here we choose `px': useful for screen-destined SVGs, and fewer bug reports
             * about "not the same numbers as what's in the SVG file" (at least for documents
             * without a viewBox attribute on the root <svg> element).  Similarly, it's also
             * the most reliable unit (i.e. least likely to be wrong in different viewing
             * conditions) for viewBox-less SVG files given that it's the unit that inkscape
             * uses for all coordinates.
             *
             * For documents that do have a viewBox attribute on the root <svg> element, it
             * might be better if we used either viewBox coordinates or if we used the unit of
             * say the width attribute of the root <svg> element.  However, these pose problems
             * in that they aren't in general absolute units as currently required by
             * doc_units.
             */
            SPUnit const *new_unit = &sp_unit_get_by_id(SP_UNIT_PX);

            if (value) {
                SPUnit const *const req_unit = sp_unit_get_by_abbreviation(value);
                if ( req_unit == NULL ) {
                    g_warning("Unrecognized unit `%s'", value);
                    /* fixme: Document errors should be reported in the status bar or
                     * the like (e.g. as per
                     * http://www.w3.org/TR/SVG11/implnote.html#ErrorProcessing); g_log
                     * should be only for programmer errors. */
                } else if ( req_unit->base == SP_UNIT_ABSOLUTE ||
                            req_unit->base == SP_UNIT_DEVICE     ) {
                    new_unit = req_unit;
                } else {
                    g_warning("Document units must be absolute like `mm', `pt' or `px', but found `%s'",
                              value);
                    /* fixme: Don't use g_log (see above). */
                }
            }
            nv->doc_units = new_unit;
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    }
    case SP_ATTR_UNITS: {
            SPUnit const *new_unit = NULL;

            if (value) {
                SPUnit const *const req_unit = sp_unit_get_by_abbreviation(value);
                if ( req_unit == NULL ) {
                    g_warning("Unrecognized unit `%s'", value);
                    /* fixme: Document errors should be reported in the status bar or
                     * the like (e.g. as per
                     * http://www.w3.org/TR/SVG11/implnote.html#ErrorProcessing); g_log
                     * should be only for programmer errors. */
                } else if ( req_unit->base == SP_UNIT_ABSOLUTE ||
                            req_unit->base == SP_UNIT_DEVICE     ) {
                    new_unit = req_unit;
                } else {
                    g_warning("Document units must be absolute like `mm', `pt' or `px', but found `%s'",
                              value);
                    /* fixme: Don't use g_log (see above). */
                }
            }
            nv->units = new_unit;
            object->requestModified(SP_OBJECT_MODIFIED_FLAG);
            break;
    }
    default:
            if (((SPObjectClass *) (parent_class))->set) {
                ((SPObjectClass *) (parent_class))->set(object, key, value);
            }
            break;
    }
}

/**
* add a grid item from SVG-repr. Check if this namedview already has a gridobject for this one! If desktop=null, add grid-canvasitem to all desktops of this namedview,
* otherwise only add it to the specified desktop.
*/
static Inkscape::CanvasGrid*
sp_namedview_add_grid(SPNamedView *nv, Inkscape::XML::Node *repr, SPDesktop *desktop) {
    Inkscape::CanvasGrid* grid = NULL;
    //check if namedview already has an object for this grid
    for (GSList *l = nv->grids; l != NULL; l = l->next) {
        Inkscape::CanvasGrid* g = (Inkscape::CanvasGrid*) l->data;
        if (repr == g->repr) {
            grid = g;
            break;
        }
    }

    if (!grid) {
        //create grid object
        Inkscape::GridType gridtype = Inkscape::CanvasGrid::getGridTypeFromSVGName(repr->attribute("type"));
        if (!nv->document) {
            g_warning("sp_namedview_add_grid - how come doc is null here?!");
            return NULL;
        }
        grid = Inkscape::CanvasGrid::NewGrid(nv, repr, nv->document, gridtype);
        nv->grids = g_slist_append(nv->grids, grid);
    }

    if (!desktop) {
        //add canvasitem to all desktops
        for (GSList *l = nv->views; l != NULL; l = l->next) {
            SPDesktop *dt = static_cast<SPDesktop*>(l->data);
            grid->createCanvasItem(dt);
        }
    } else {
        //add canvasitem only for specified desktop
        grid->createCanvasItem(desktop);
    }

    return grid;
}

static void sp_namedview_child_added(SPObject *object, Inkscape::XML::Node *child, Inkscape::XML::Node *ref)
{
    SPNamedView *nv = (SPNamedView *) object;

    if (((SPObjectClass *) (parent_class))->child_added) {
        (* ((SPObjectClass *) (parent_class))->child_added)(object, child, ref);
    }

    if (!strcmp(child->name(), "inkscape:grid")) {
        sp_namedview_add_grid(nv, child, NULL);
    } else {
        SPObject *no = object->document->getObjectByRepr(child);
        if ( !SP_IS_OBJECT(no) )
            return;

        if (SP_IS_GUIDE(no)) {
            SPGuide *g = (SPGuide *) no;
            nv->guides = g_slist_prepend(nv->guides, g);
            g_object_set(G_OBJECT(g), "color", nv->guidecolor, "hicolor", nv->guidehicolor, NULL);
            if (nv->editable) {
                for (GSList *l = nv->views; l != NULL; l = l->next) {
                    g->SPGuide::showSPGuide(static_cast<SPDesktop*>(l->data)->guides, (GCallback) sp_dt_guide_event);
                    if (static_cast<SPDesktop*>(l->data)->guides_active)
                        g->sensitize(sp_desktop_canvas(static_cast<SPDesktop*> (l->data)),
                                           TRUE);
                    sp_namedview_show_single_guide(SP_GUIDE(g), nv->showguides);
                }
            }
        }
    }
}

static void sp_namedview_remove_child(SPObject *object, Inkscape::XML::Node *child)
{
    SPNamedView *nv = (SPNamedView *) object;

    if (!strcmp(child->name(), "inkscape:grid")) {
        for ( GSList *iter = nv->grids ; iter ; iter = iter->next ) {
            Inkscape::CanvasGrid *gr = (Inkscape::CanvasGrid *)iter->data;
            if ( gr->repr == child ) {
                delete gr;
                nv->grids = g_slist_remove_link(nv->grids, iter);
                break;
            }
        }
    } else {
        GSList **ref = &nv->guides;
        for ( GSList *iter = nv->guides ; iter ; iter = iter->next ) {
            if ( reinterpret_cast<SPObject *>(iter->data)->getRepr() == child ) {
                *ref = iter->next;
                iter->next = NULL;
                g_slist_free_1(iter);
                break;
            }
            ref = &iter->next;
        }
    }

    if (((SPObjectClass *) (parent_class))->remove_child) {
        (* ((SPObjectClass *) (parent_class))->remove_child)(object, child);
    }
}

static Inkscape::XML::Node *sp_namedview_write(SPObject *object, Inkscape::XML::Document *doc, Inkscape::XML::Node *repr, guint flags)
{
    if ( ( flags & SP_OBJECT_WRITE_EXT ) &&
         repr != object->getRepr() )
    {
        if (repr) {
            repr->mergeFrom(object->getRepr(), "id");
        } else {
            repr = object->getRepr()->duplicate(doc);
        }
    }

    return repr;
}

void SPNamedView::show(SPDesktop *desktop)
{
    for (GSList *l = guides; l != NULL; l = l->next) {
        SP_GUIDE(l->data)->showSPGuide( desktop->guides, (GCallback) sp_dt_guide_event);
        if (desktop->guides_active) {
            SP_GUIDE(l->data)->sensitize(sp_desktop_canvas(desktop), TRUE);
        }
        sp_namedview_show_single_guide(SP_GUIDE(l->data), showguides);
    }

    views = g_slist_prepend(views, desktop);

    // generate grids specified in SVG:
    Inkscape::XML::Node *repr = this->getRepr();
    if (repr) {
        for (Inkscape::XML::Node * child = repr->firstChild() ; child != NULL; child = child->next() ) {
            if (!strcmp(child->name(), "inkscape:grid")) {
                sp_namedview_add_grid(this, child, desktop);
            }
        }
    }

    desktop->showGrids(grids_visible, false);
}

#define MIN_ONSCREEN_DISTANCE 50

void SPNamedView::writeNewGrid(SPDocument *document,int gridtype)
{
    g_assert(this->getRepr() != NULL);
    Inkscape::CanvasGrid::writeNewGridToRepr(this->getRepr(),document,static_cast<Inkscape::GridType>(gridtype));
}

/*
 * Restores window geometry from the document settings or defaults in prefs
 */
void sp_namedview_window_from_document(SPDesktop *desktop)
{
    SPNamedView *nv = desktop->namedview;
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool geometry_from_file = (1 == prefs->getInt("/options/savewindowgeometry/value", 0));
    bool show_dialogs = TRUE;

    // restore window size and position stored with the document
    if (geometry_from_file) {
        if (nv->window_maximized) {
            Gtk::Window *win = desktop->getToplevel();
            if (win){
                win->maximize();
            }
        } else {
            gint w = MIN(gdk_screen_width(), nv->window_width);
            gint h = MIN(gdk_screen_height(), nv->window_height);
            // prevent the window from moving off the screen to the right or to the bottom
            gint x = MIN(gdk_screen_width() - MIN_ONSCREEN_DISTANCE, nv->window_x);
            gint y = MIN(gdk_screen_height() - MIN_ONSCREEN_DISTANCE, nv->window_y);
            // prevent the window from moving off the screen to the left or to the top
            x = MAX(MIN_ONSCREEN_DISTANCE - nv->window_width, x);
            y = MAX(MIN_ONSCREEN_DISTANCE - nv->window_height, y);
            if (w>0 && h>0) {

                #ifndef WIN32
                gint dx, dy, dw, dh;
                desktop->getWindowGeometry(dx, dy, dw, dh);
                if (w != dw || h != dh) {
                    // Don't show dialogs when window is initially resized on OSX/Linux due to gdl dock bug
                    // This will happen on sp_desktop_widget_size_allocate
                    show_dialogs = FALSE;
                }
                #endif

                desktop->setWindowSize(w, h);
                desktop->setWindowPosition(Geom::Point(x, y));

            }
        }

    }

    // restore zoom and view
    if (nv->zoom != 0 && nv->zoom != HUGE_VAL && !IS_NAN(nv->zoom)
        && nv->cx != HUGE_VAL && !IS_NAN(nv->cx)
        && nv->cy != HUGE_VAL && !IS_NAN(nv->cy)) {
        desktop->zoom_absolute(nv->cx, nv->cy, nv->zoom);
    } else if (sp_desktop_document(desktop)) { // document without saved zoom, zoom to its page
        desktop->zoom_page();
    }

    // cancel any history of zooms up to this point
    desktop->zooms_past.clear();

    if (show_dialogs) {
        desktop->show_dialogs();
    }
}

bool SPNamedView::getSnapGlobal() const
{
    return this->snap_manager.snapprefs.getSnapEnabledGlobally();
}

void SPNamedView::setSnapGlobal(bool v)
{
    g_assert(this->getRepr() != NULL);
    sp_repr_set_boolean(this->getRepr(), "inkscape:snap-global", v);
}

void sp_namedview_update_layers_from_document (SPDesktop *desktop)
{
    SPObject *layer = NULL;
    SPDocument *document = desktop->doc();
    SPNamedView *nv = desktop->namedview;
    if ( nv->default_layer_id != 0 ) {
        layer = document->getObjectById(g_quark_to_string(nv->default_layer_id));
    }
    // don't use that object if it's not at least group
    if ( !layer || !SP_IS_GROUP(layer) ) {
        layer = NULL;
    }
    // if that didn't work out, look for the topmost layer
    if (!layer) {
        for ( SPObject *iter = document->getRoot()->firstChild(); iter ; iter = iter->getNext() ) {
            if (desktop->isLayer(iter)) {
                layer = iter;
            }
        }
    }
    if (layer) {
        desktop->setCurrentLayer(layer);
    }

    // FIXME: find a better place to do this
    desktop->event_log->updateUndoVerbs();
}

void sp_namedview_document_from_window(SPDesktop *desktop)
{
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    bool save_geometry_in_file = (1 == prefs->getInt("/options/savewindowgeometry/value", 0));
    bool save_viewport_in_file = prefs->getBool("/options/savedocviewport/value", true);
    Inkscape::XML::Node *view = desktop->namedview->getRepr();
    Geom::Rect const r = desktop->get_display_area();

    // saving window geometry is not undoable
    bool saved = DocumentUndo::getUndoSensitive(sp_desktop_document(desktop));
    DocumentUndo::setUndoSensitive(sp_desktop_document(desktop), false);

    if (save_viewport_in_file) {
        sp_repr_set_svg_double(view, "inkscape:zoom", desktop->current_zoom());
        sp_repr_set_svg_double(view, "inkscape:cx", r.midpoint()[Geom::X]);
        sp_repr_set_svg_double(view, "inkscape:cy", r.midpoint()[Geom::Y]);
    }

    if (save_geometry_in_file) {
        gint w, h, x, y;
        desktop->getWindowGeometry(x, y, w, h);
        sp_repr_set_int(view, "inkscape:window-width", w);
        sp_repr_set_int(view, "inkscape:window-height", h);
        sp_repr_set_int(view, "inkscape:window-x", x);
        sp_repr_set_int(view, "inkscape:window-y", y);
        sp_repr_set_int(view, "inkscape:window-maximized", desktop->is_maximized());
    }

    view->setAttribute("inkscape:current-layer", desktop->currentLayer()->getId());

    // restore undoability
    DocumentUndo::setUndoSensitive(sp_desktop_document(desktop), saved);
}

void SPNamedView::hide(SPDesktop const *desktop)
{
    g_assert(desktop != NULL);
    g_assert(g_slist_find(views, desktop));

    for (GSList *l = guides; l != NULL; l = l->next) {
        SP_GUIDE(l->data)->hideSPGuide(sp_desktop_canvas(desktop));
    }

    views = g_slist_remove(views, desktop);
}

void SPNamedView::activateGuides(gpointer desktop, gboolean active)
{
    g_assert(desktop != NULL);
    g_assert(g_slist_find(views, desktop));

    SPDesktop *dt = static_cast<SPDesktop*>(desktop);

    for (GSList *l = guides; l != NULL; l = l->next) {
        SP_GUIDE(l->data)->sensitize( sp_desktop_canvas(dt), active);
    }
}

static void sp_namedview_setup_guides(SPNamedView *nv)
{
    for (GSList *l = nv->guides; l != NULL; l = l->next) {
        sp_namedview_show_single_guide(SP_GUIDE(l->data), nv->showguides);
    }
}

static void sp_namedview_show_single_guide(SPGuide* guide, bool show)
{
    for (GSList *v = guide->views; v != NULL; v = v->next) {
        if (show) {
            sp_canvas_item_show(SP_CANVAS_ITEM(v->data));
            sp_canvas_item_show(SP_CANVAS_ITEM(SP_GUIDELINE(v->data)->origin));
        } else {
            sp_canvas_item_hide(SP_CANVAS_ITEM(v->data));
            sp_canvas_item_hide(SP_CANVAS_ITEM(SP_GUIDELINE(v->data)->origin));
        }
    }
}

void sp_namedview_toggle_guides(SPDocument *doc, Inkscape::XML::Node *repr)
{
    unsigned int v;
    unsigned int set = sp_repr_get_boolean(repr, "showguides", &v);
    if (!set) { // hide guides if not specified, for backwards compatibility
        v = FALSE;
    } else {
        v = !v;
    }

    bool saved = DocumentUndo::getUndoSensitive(doc);
    DocumentUndo::setUndoSensitive(doc, false);
    sp_repr_set_boolean(repr, "showguides", v);
    DocumentUndo::setUndoSensitive(doc, saved);

    doc->setModifiedSinceSave();
}

void sp_namedview_show_grids(SPNamedView * namedview, bool show, bool dirty_document)
{
    namedview->grids_visible = show;

    SPDocument *doc = namedview->document;
    Inkscape::XML::Node *repr = namedview->getRepr();

    bool saved = DocumentUndo::getUndoSensitive(doc);
    DocumentUndo::setUndoSensitive(doc, false);
    sp_repr_set_boolean(repr, "showgrid", namedview->grids_visible);
    DocumentUndo::setUndoSensitive(doc, saved);

    /* we don't want the document to get dirty on startup; that's when
       we call this function with dirty_document = false */
    if (dirty_document) {
        doc->setModifiedSinceSave();
    }
}

gchar const *SPNamedView::getName() const
{
    SPException ex;
    SP_EXCEPTION_INIT(&ex);
    return this->getAttribute("id", &ex);
}

guint SPNamedView::getViewCount()
{
    return ++viewcount;
}

GSList const *SPNamedView::getViewList() const
{
    return views;
}

/* This should be moved somewhere */

static gboolean sp_str_to_bool(const gchar *str)
{
    if (str) {
        if (!g_ascii_strcasecmp(str, "true") ||
            !g_ascii_strcasecmp(str, "yes") ||
            !g_ascii_strcasecmp(str, "y") ||
            (atoi(str) != 0)) {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean sp_nv_read_opacity(const gchar *str, guint32 *color)
{
    if (!str) {
        return FALSE;
    }

    gchar *u;
    gdouble v = g_ascii_strtod(str, &u);
    if (!u) {
        return FALSE;
    }
    v = CLAMP(v, 0.0, 1.0);

    *color = (*color & 0xffffff00) | (guint32) floor(v * 255.9999);

    return TRUE;
}

SPNamedView *sp_document_namedview(SPDocument *document, const gchar *id)
{
    g_return_val_if_fail(document != NULL, NULL);

    SPObject *nv = sp_item_group_get_child_by_name(document->getRoot(), NULL, "sodipodi:namedview");
    g_assert(nv != NULL);

    if (id == NULL) {
        return (SPNamedView *) nv;
    }

    while (nv && strcmp(nv->getId(), id)) {
        nv = sp_item_group_get_child_by_name(document->getRoot(), nv, "sodipodi:namedview");
    }

    return (SPNamedView *) nv;
}

void SPNamedView::setGuides(bool v)
{
    g_assert(this->getRepr() != NULL);
    sp_repr_set_boolean(this->getRepr(), "showguides", v);
    sp_repr_set_boolean(this->getRepr(), "inkscape:guide-bbox", v);
}

bool SPNamedView::getGuides()
{
    g_assert(this->getRepr() != NULL);
    unsigned int v;
    unsigned int set = sp_repr_get_boolean(this->getRepr(), "showguides", &v);
    if (!set) { // hide guides if not specified, for backwards compatibility
        v = FALSE;
    }

    return v;
}
/**
 * Gets page fitting margin information from the namedview node in the XML.
 * \param nv_repr reference to this document's namedview
 * \param key the same key used by the RegisteredScalarUnit in
 *        ui/widget/page-sizer.cpp
 * \param margin_units units for the margin
 * \param return_units units to return the result in
 * \param width width in px (for percentage margins)
 * \param height height in px (for percentage margins)
 * \param use_width true if the this key is left or right margins, false
 *        otherwise.  Used for percentage margins.
 * \return the margin size in px, else 0.0 if anything is invalid.
 */
double SPNamedView::getMarginLength(gchar const * const key,
                             SPUnit const * const margin_units,
                             SPUnit const * const return_units,
                             double const width,
                             double const height,
                             bool const use_width)
{
    double value;
    if(!this->storeAsDouble(key,&value)) {
        return 0.0;
    }
    if (margin_units == &sp_unit_get_by_id (SP_UNIT_PERCENT)) {
        return (use_width)? width * value : height * value; 
    }
    if (!sp_convert_distance (&value, margin_units, return_units)) {
        return 0.0;
    }
    return value;
}


/**
 * Returns namedview's default metric.
 */
SPMetric SPNamedView::getDefaultMetric() const
{
    if (doc_units) {
        return sp_unit_get_metric(doc_units);
    } else {
        return SP_PT;
    }
}

/**
 * Returns the first grid it could find that isEnabled(). Returns NULL, if none is enabled
 */
Inkscape::CanvasGrid * sp_namedview_get_first_enabled_grid(SPNamedView *namedview)
{
    for (GSList const * l = namedview->grids; l != NULL; l = l->next) {
        Inkscape::CanvasGrid * grid = (Inkscape::CanvasGrid*) l->data;
        if (grid->isEnabled())
            return grid;
    }

    return NULL;
}

void SPNamedView::translateGuides(Geom::Translate const &tr) {
    for (GSList *l = guides; l != NULL; l = l->next) {
        SPGuide &guide = *SP_GUIDE(l->data);
        Geom::Point point_on_line = guide.point_on_line;
        point_on_line *= tr;
        sp_guide_moveto(guide, point_on_line, true);
    }
}

void SPNamedView::translateGrids(Geom::Translate const &tr) {
    for (GSList *l = grids; l != NULL; l = l->next) {
        Inkscape::CanvasGrid* g = reinterpret_cast<Inkscape::CanvasGrid*>(l->data);
        if (g) {
            g->setOrigin(g->origin * tr);
        }
    }
}

void SPNamedView::scrollAllDesktops(double dx, double dy, bool is_scrolling) {
        for(GSList *l = views; l; l = l->next) {
            SPDesktop *desktop = static_cast<SPDesktop *>(l->data);
            desktop->scroll_world_in_svg_coords(dx, dy, is_scrolling);
        }
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
