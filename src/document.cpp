/*
 * SPDocument manipulation
 *
 * Authors:
 *   Lauris Kaplinski <lauris@kaplinski.com>
 *   MenTaLguY <mental@rydia.net>
 *   bulia byak <buliabyak@users.sf.net>
 *   Jon A. Cruz <jon@joncruz.org>
 *   Abhishek Sharma
 *   Tavmjong Bah <tavmjong@free.fr>
 *
 * Copyright (C) 2004-2005 MenTaLguY
 * Copyright (C) 1999-2002 Lauris Kaplinski
 * Copyright (C) 2000-2001 Ximian, Inc.
 * Copyright (C) 2012 Tavmjong Bah
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */

/** \class SPDocument
 * SPDocument serves as the container of both model trees (agnostic XML
 * and typed object tree), and implements all of the document-level
 * functionality used by the program. Many document level operations, like
 * load, save, print, export and so on, use SPDocument as their basic datatype.
 *
 * SPDocument implements undo and redo stacks and an id-based object
 * dictionary.  Thanks to unique id attributes, the latter can be used to
 * map from the XML tree back to the object tree.
 *
 * SPDocument performs the basic operations needed for asynchronous
 * update notification (SPObject ::modified virtual method), and implements
 * the 'modified' signal, as well.
 */


#define noSP_DOCUMENT_DEBUG_IDLE
#define noSP_DOCUMENT_DEBUG_UNDO

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <string>
#include <cstring>
#include <2geom/transforms.h>

#include "widgets/desktop-widget.h"
#include "desktop.h"
#include "dir-util.h"
#include "display/drawing-item.h"
#include "document-private.h"
#include "document-undo.h"
#include "helper/units.h"
#include "id-clash.h"
#include "inkscape-private.h"
#include "inkscape-version.h"
#include "libavoid/router.h"
#include "persp3d.h"
#include "preferences.h"
#include "profile-manager.h"
#include "rdf.h"
#include "sp-item-group.h"
#include "sp-namedview.h"
#include "sp-object-repr.h"
#include "sp-symbol.h"
#include "transf_mat_3x4.h"
#include "unit-constants.h"
#include "xml/repr.h"
#include "xml/rebase-hrefs.h"
#include "libcroco/cr-cascade.h"

using Inkscape::DocumentUndo;

// Higher number means lower priority.
#define SP_DOCUMENT_UPDATE_PRIORITY (G_PRIORITY_HIGH_IDLE - 2)

// Should have a lower priority than SP_DOCUMENT_UPDATE_PRIORITY,
// since we want it to happen when there are no more updates.
#define SP_DOCUMENT_REROUTING_PRIORITY (G_PRIORITY_HIGH_IDLE - 1)


static gint sp_document_idle_handler(gpointer data);
static gint sp_document_rerouting_handler(gpointer data);

gboolean sp_document_resource_list_free(gpointer key, gpointer value, gpointer data);

static gint doc_count = 0;

static unsigned long next_serial = 0;

SPDocument::SPDocument() :
    keepalive(FALSE),
    virgin(TRUE),
    modified_since_save(FALSE),
    rdoc(0),
    rroot(0),
    root(0),
    style_cascade(cr_cascade_new(NULL, NULL, NULL)),
    uri(0),
    base(0),
    name(0),
    priv(0), // reset in ctor
    actionkey(),
    modified_id(0),
    rerouting_handler_id(0),
    profileManager(0), // deferred until after other initialization
    router(new Avoid::Router(Avoid::PolyLineRouting|Avoid::OrthogonalRouting)),
    _collection_queue(0),
    oldSignalsConnected(false),
    current_persp3d(NULL),
    current_persp3d_impl(NULL)
{
    // Penalise libavoid for choosing paths with needless extra segments.
    // This results in much better looking orthogonal connector paths.
    router->setRoutingPenalty(Avoid::segmentPenalty);

    SPDocumentPrivate *p = new SPDocumentPrivate();

    p->serial = next_serial++;

    p->iddef = g_hash_table_new(g_direct_hash, g_direct_equal);
    p->reprdef = g_hash_table_new(g_direct_hash, g_direct_equal);

    p->resources = g_hash_table_new(g_str_hash, g_str_equal);

    p->sensitive = FALSE;
    p->partial = NULL;
    p->history_size = 0;
    p->undo = NULL;
    p->redo = NULL;
    p->seeking = false;

    priv = p;

    // Once things are set, hook in the manager
    profileManager = new Inkscape::ProfileManager(this);

    // XXX only for testing!
    priv->undoStackObservers.add(p->console_output_undo_observer);
}

SPDocument::~SPDocument() {
    collectOrphans();

    // kill/unhook this first
    if ( profileManager ) {
        delete profileManager;
        profileManager = 0;
    }

    if (router) {
        delete router;
        router = NULL;
    }

    if (priv) {
        if (priv->partial) {
            sp_repr_free_log(priv->partial);
            priv->partial = NULL;
        }

        DocumentUndo::clearRedo(this);
        DocumentUndo::clearUndo(this);

        if (root) {
            root->releaseReferences();
            sp_object_unref(root);
            root = NULL;
        }

        if (priv->iddef) g_hash_table_destroy(priv->iddef);
        if (priv->reprdef) g_hash_table_destroy(priv->reprdef);

        if (rdoc) Inkscape::GC::release(rdoc);

        /* Free resources */
        g_hash_table_foreach_remove(priv->resources, sp_document_resource_list_free, this);
        g_hash_table_destroy(priv->resources);

        delete priv;
        priv = NULL;
    }

    cr_cascade_unref(style_cascade);
    style_cascade = NULL;

    if (name) {
        g_free(name);
        name = NULL;
    }
    if (base) {
        g_free(base);
        base = NULL;
    }
    if (uri) {
        g_free(uri);
        uri = NULL;
    }

    if (modified_id) {
        g_source_remove(modified_id);
        modified_id = 0;
    }

    if (rerouting_handler_id) {
        g_source_remove(rerouting_handler_id);
        rerouting_handler_id = 0;
    }

    if (oldSignalsConnected) {
        g_signal_handlers_disconnect_by_func(G_OBJECT(INKSCAPE),
                                             reinterpret_cast<gpointer>(DocumentUndo::resetKey),
                                             static_cast<gpointer>(this));
    } else {
        _selection_changed_connection.disconnect();
        _desktop_activated_connection.disconnect();
    }

    if (keepalive) {
        inkscape_unref();
        keepalive = FALSE;
    }
    //delete this->_whiteboard_session_manager;
}

SPDefs *SPDocument::getDefs()
{
    if (!root) {
        return NULL;
    }
    return root->defs;
}

Persp3D *SPDocument::getCurrentPersp3D() {
    // Check if current_persp3d is still valid
    std::vector<Persp3D*> plist;
    getPerspectivesInDefs(plist);
    for (unsigned int i = 0; i < plist.size(); ++i) {
        if (current_persp3d == plist[i])
            return current_persp3d;
    }

    // If not, return the first perspective in defs (which may be NULL of none exists)
    current_persp3d = persp3d_document_first_persp (this);

    return current_persp3d;
}

Persp3DImpl *SPDocument::getCurrentPersp3DImpl() {
    return current_persp3d_impl;
}

void SPDocument::setCurrentPersp3D(Persp3D * const persp) {
    current_persp3d = persp;
    //current_persp3d_impl = persp->perspective_impl;
}

void SPDocument::getPerspectivesInDefs(std::vector<Persp3D*> &list) const
{
    for (SPObject *i = root->defs->firstChild(); i; i = i->getNext() ) {
        if (SP_IS_PERSP3D(i)) {
            list.push_back(SP_PERSP3D(i));
        }
    }
}

/**
void SPDocument::initialize_current_persp3d()
{
    this->current_persp3d = persp3d_document_first_persp(this);
    if (!this->current_persp3d) {
        this->current_persp3d = persp3d_create_xml_element(this);
    }
}
**/

unsigned long SPDocument::serial() const {
    return priv->serial;
}

void SPDocument::queueForOrphanCollection(SPObject *object) {
    g_return_if_fail(object != NULL);
    g_return_if_fail(object->document == this);

    sp_object_ref(object, NULL);
    _collection_queue = g_slist_prepend(_collection_queue, object);
}

void SPDocument::collectOrphans() {
    while (_collection_queue) {
        GSList *objects=_collection_queue;
        _collection_queue = NULL;
        for ( GSList *iter=objects ; iter ; iter = iter->next ) {
            SPObject *object=reinterpret_cast<SPObject *>(iter->data);
            object->collectOrphan();
            sp_object_unref(object, NULL);
        }
        g_slist_free(objects);
    }
}

void SPDocument::reset_key (void */*dummy*/)
{
    actionkey.clear();
}

SPDocument *SPDocument::createDoc(Inkscape::XML::Document *rdoc,
                                  gchar const *uri,
                                  gchar const *base,
                                  gchar const *name,
                                  unsigned int keepalive)
{
    SPDocument *document = new SPDocument();

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    Inkscape::XML::Node *rroot = rdoc->root();

    document->keepalive = keepalive;

    document->rdoc = rdoc;
    document->rroot = rroot;

    if (document->uri){
        g_free(document->uri);
        document->uri = 0;
    }
    if (document->base){
        g_free(document->base);
        document->base = 0;
    }
    if (document->name){
        g_free(document->name);
        document->name = 0;
    }
#ifndef WIN32
    document->uri = prepend_current_dir_if_relative(uri);
#else
    // FIXME: it may be that prepend_current_dir_if_relative works OK on windows too, test!
    document->uri = uri? g_strdup(uri) : NULL;
#endif

    // base is simply the part of the path before filename; e.g. when running "inkscape ../file.svg" the base is "../"
    // which is why we use g_get_current_dir() in calculating the abs path above
    //This is NULL for a new document
    if (base) {
        document->base = g_strdup(base);
    } else {
        document->base = NULL;
    }
    document->name = g_strdup(name);

    document->root = sp_object_repr_build_tree(document, rroot);

    /* fixme: Not sure about this, but lets assume ::build updates */
    rroot->setAttribute("inkscape:version", Inkscape::version_string);
    /* fixme: Again, I moved these here to allow version determining in ::build (Lauris) */

    /* Quick hack 2 - get default image size into document */
    if (!rroot->attribute("width")) rroot->setAttribute("width", "100%");
    if (!rroot->attribute("height")) rroot->setAttribute("height", "100%");
    /* End of quick hack 2 */

    /* Quick hack 3 - Set uri attributes */
//    if (uri) {					// this is done in do_change_uri()
//        rroot->setAttribute("sodipodi:docname", uri);
//    }
    /* End of quick hack 3 */

    /* Eliminate obsolete sodipodi:docbase, for privacy reasons */
    rroot->setAttribute("sodipodi:docbase", NULL);

    /* Eliminate any claim to adhere to a profile, as we don't try to */
    rroot->setAttribute("baseProfile", NULL);

    // creating namedview
    if (!sp_item_group_get_child_by_name(document->root, NULL, "sodipodi:namedview")) {
        // if there's none in the document already,
        Inkscape::XML::Node *rnew = NULL;

        rnew = rdoc->createElement("sodipodi:namedview");
        //rnew->setAttribute("id", "base");

        // Add namedview data from the preferences
        // we can't use getAllEntries because this could produce non-SVG doubles
        Glib::ustring pagecolor = prefs->getString("/template/base/pagecolor");
        if (!pagecolor.empty()) {
            rnew->setAttribute("pagecolor", pagecolor.data());
        }
        Glib::ustring bordercolor = prefs->getString("/template/base/bordercolor");
        if (!bordercolor.empty()) {
            rnew->setAttribute("bordercolor", bordercolor.data());
        }
        sp_repr_set_svg_double(rnew, "borderopacity",
            prefs->getDouble("/template/base/borderopacity", 1.0));
        sp_repr_set_svg_double(rnew, "objecttolerance",
            prefs->getDouble("/template/base/objecttolerance", 10.0));
        sp_repr_set_svg_double(rnew, "gridtolerance",
            prefs->getDouble("/template/base/gridtolerance", 10.0));
        sp_repr_set_svg_double(rnew, "guidetolerance",
            prefs->getDouble("/template/base/guidetolerance", 10.0));
        sp_repr_set_svg_double(rnew, "inkscape:pageopacity",
            prefs->getDouble("/template/base/inkscape:pageopacity", 0.0));
        sp_repr_set_int(rnew, "inkscape:pageshadow",
            prefs->getInt("/template/base/inkscape:pageshadow", 2));
        sp_repr_set_int(rnew, "inkscape:window-width",
            prefs->getInt("/template/base/inkscape:window-width", 640));
        sp_repr_set_int(rnew, "inkscape:window-height",
            prefs->getInt("/template/base/inkscape:window-height", 480));

        // insert into the document
        rroot->addChild(rnew, NULL);
        // clean up
        Inkscape::GC::release(rnew);
    }

    // Defs
    if (!document->root->defs) {
        Inkscape::XML::Node *r = rdoc->createElement("svg:defs");
        rroot->addChild(r, NULL);
        Inkscape::GC::release(r);
        g_assert(document->root->defs);
    }

    /* Default RDF */
    rdf_set_defaults( document );

    if (keepalive) {
        inkscape_ref();
    }

    // Check if the document already has a perspective (e.g., when opening an existing
    // document). If not, create a new one and set it as the current perspective.
    document->setCurrentPersp3D(persp3d_document_first_persp(document));
    if (!document->getCurrentPersp3D()) {
        //document->setCurrentPersp3D(persp3d_create_xml_element (document));
        Persp3DImpl *persp_impl = new Persp3DImpl();
        document->setCurrentPersp3DImpl(persp_impl);
    }

    DocumentUndo::setUndoSensitive(document, true);

    // reset undo key when selection changes, so that same-key actions on different objects are not coalesced
    g_signal_connect(G_OBJECT(INKSCAPE), "change_selection",
                     G_CALLBACK(DocumentUndo::resetKey), document);
    g_signal_connect(G_OBJECT(INKSCAPE), "activate_desktop",
                     G_CALLBACK(DocumentUndo::resetKey), document);
    document->oldSignalsConnected = true;

    return document;
}

/**
 * Fetches document from URI, or creates new, if NULL; public document
 * appears in document list.
 */
SPDocument *SPDocument::createNewDoc(gchar const *uri, unsigned int keepalive, bool make_new)
{
    SPDocument *doc;
    Inkscape::XML::Document *rdoc;
    gchar *base = NULL;
    gchar *name = NULL;

    if (uri) {
        Inkscape::XML::Node *rroot;
        gchar *s, *p;
        /* Try to fetch repr from file */
        rdoc = sp_repr_read_file(uri, SP_SVG_NS_URI);
        /* If file cannot be loaded, return NULL without warning */
        if (rdoc == NULL) return NULL;
        rroot = rdoc->root();
        /* If xml file is not svg, return NULL without warning */
        /* fixme: destroy document */
        if (strcmp(rroot->name(), "svg:svg") != 0) return NULL;
        s = g_strdup(uri);
        p = strrchr(s, '/');
        if (p) {
            name = g_strdup(p + 1);
            p[1] = '\0';
            base = g_strdup(s);
        } else {
            base = NULL;
            name = g_strdup(uri);
        }
        g_free(s);
    } else {
        rdoc = sp_repr_document_new("svg:svg");
    }

    if (make_new) {
        base = NULL;
        uri = NULL;
        name = g_strdup_printf(_("New document %d"), ++doc_count);
    }

    //# These should be set by now
    g_assert(name);

    doc = createDoc(rdoc, uri, base, name, keepalive);

    g_free(base);
    g_free(name);

    return doc;
}

SPDocument *SPDocument::createNewDocFromMem(gchar const *buffer, gint length, unsigned int keepalive)
{
    SPDocument *doc = 0;

    Inkscape::XML::Document *rdoc = sp_repr_read_mem(buffer, length, SP_SVG_NS_URI);
    if ( rdoc ) {
        // Only continue to create a non-null doc if it could be loaded
        Inkscape::XML::Node *rroot = rdoc->root();
        if ( strcmp(rroot->name(), "svg:svg") != 0 ) {
            // If xml file is not svg, return NULL without warning
            // TODO fixme: destroy document
        } else {
            Glib::ustring name = Glib::ustring::compose( _("Memory document %1"), ++doc_count );
            doc = createDoc(rdoc, NULL, NULL, name.c_str(), keepalive);
        }
    }

    return doc;
}

SPDocument *SPDocument::doRef()
{
    Inkscape::GC::anchor(this);
    return this;
}

SPDocument *SPDocument::doUnref()
{
    Inkscape::GC::release(this);
    return NULL;
}

gdouble SPDocument::getWidth() const
{
    g_return_val_if_fail(this->priv != NULL, 0.0);
    g_return_val_if_fail(this->root != NULL, 0.0);

    gdouble result = root->width.computed;
    if (root->width.unit == SVGLength::PERCENT && root->viewBox_set) {
        result = root->viewBox.width();
    }
    return result;
}

void SPDocument::setWidth(gdouble width, const SPUnit *unit)
{
    if (root->width.unit == SVGLength::PERCENT && root->viewBox_set) { // set to viewBox=
        root->viewBox.setMax(Geom::Point(root->viewBox.left() + sp_units_get_pixels (width, *unit), root->viewBox.bottom()));
    } else { // set to width=
        gdouble old_computed = root->width.computed;
        root->width.computed = sp_units_get_pixels (width, *unit);
        /* SVG does not support meters as a unit, so we must translate meters to
         * cm when writing */
        if (!strcmp(unit->abbr, "m")) {
            root->width.value = 100*width;
            root->width.unit = SVGLength::CM;
        } else {
            root->width.value = width;
            root->width.unit = (SVGLength::Unit) sp_unit_get_svg_unit(unit);
        }

        if (root->viewBox_set)
            root->viewBox.setMax(Geom::Point(root->viewBox.left() + (root->width.computed / old_computed) * root->viewBox.width(), root->viewBox.bottom()));
    }

    root->updateRepr();
}

gdouble SPDocument::getHeight() const
{
    g_return_val_if_fail(this->priv != NULL, 0.0);
    g_return_val_if_fail(this->root != NULL, 0.0);

    gdouble result = root->height.computed;
    if (root->height.unit == SVGLength::PERCENT && root->viewBox_set) {
        result = root->viewBox.height();
    }
    return result;
}

void SPDocument::setHeight(gdouble height, const SPUnit *unit)
{
    if (root->height.unit == SVGLength::PERCENT && root->viewBox_set) { // set to viewBox=
        root->viewBox.setMax(Geom::Point(root->viewBox.right(), root->viewBox.top() + sp_units_get_pixels (height, *unit)));
    } else { // set to height=
        gdouble old_computed = root->height.computed;
        root->height.computed = sp_units_get_pixels (height, *unit);
        /* SVG does not support meters as a unit, so we must translate meters to
         * cm when writing */
        if (!strcmp(unit->abbr, "m")) {
            root->height.value = 100*height;
            root->height.unit = SVGLength::CM;
        } else {
            root->height.value = height;
            root->height.unit = (SVGLength::Unit) sp_unit_get_svg_unit(unit);
        }

        if (root->viewBox_set)
            root->viewBox.setMax(Geom::Point(root->viewBox.right(), root->viewBox.top() + (root->height.computed / old_computed) * root->viewBox.height()));
    }

    root->updateRepr();
}

Geom::Point SPDocument::getDimensions() const
{
    return Geom::Point(getWidth(), getHeight());
}

/**
 * Given a Geom::Rect that may, for example, correspond to the bbox of an object,
 * this function fits the canvas to that rect by resizing the canvas
 * and translating the document root into position.
 * \param rect fit document size to this
 * \param with_margins add margins to rect, by taking margins from this
 *        document's namedview (<sodipodi:namedview> "fit-margin-..."
 *        attributes, and "units")
 */
void SPDocument::fitToRect(Geom::Rect const &rect, bool with_margins)
{
    double const w = rect.width();
    double const h = rect.height();

    double const old_height = getHeight();
    SPUnit const &px(sp_unit_get_by_id(SP_UNIT_PX));
    
    /* in px */
    double margin_top = 0.0;
    double margin_left = 0.0;
    double margin_right = 0.0;
    double margin_bottom = 0.0;
    
    SPNamedView *nv = sp_document_namedview(this, 0);
    
    if (with_margins && nv) {
        if (nv != NULL) {
            gchar const * const units_abbr = nv->getAttribute("units");
            SPUnit const *margin_units = NULL;
            if (units_abbr != NULL) {
                margin_units = sp_unit_get_by_abbreviation(units_abbr);
            }
            if (margin_units == NULL) {
                margin_units = &px;
            }
            margin_top = nv->getMarginLength("fit-margin-top",margin_units, &px, w, h, false);
            margin_left = nv->getMarginLength("fit-margin-left",margin_units, &px, w, h, true);
            margin_right = nv->getMarginLength("fit-margin-right",margin_units, &px, w, h, true);
            margin_bottom = nv->getMarginLength("fit-margin-bottom",margin_units, &px, w, h, false);
        }
    }
    
    Geom::Rect const rect_with_margins(
            rect.min() - Geom::Point(margin_left, margin_bottom),
            rect.max() + Geom::Point(margin_right, margin_top));
    
    
    setWidth(rect_with_margins.width(), &px);
    setHeight(rect_with_margins.height(), &px);

    Geom::Translate const tr(
            Geom::Point(0, old_height - rect_with_margins.height())
            - rect_with_margins.min());
    root->translateChildItems(tr);

    if(nv) {
        Geom::Translate tr2(-rect_with_margins.min());
        nv->translateGuides(tr2);
        nv->translateGrids(tr2);

        // update the viewport so the drawing appears to stay where it was
        nv->scrollAllDesktops(-tr2[0], tr2[1], false);
    }
}

void SPDocument::setBase( gchar const* base )
{
    if (this->base) {
        g_free(this->base);
        this->base = 0;
    }
    if (base) {
        this->base = g_strdup(base);
    }
}

void SPDocument::do_change_uri(gchar const *const filename, bool const rebase)
{
    gchar *new_base = 0;
    gchar *new_name = 0;
    gchar *new_uri = 0;
    if (filename) {

#ifndef WIN32
        new_uri = prepend_current_dir_if_relative(filename);
#else
        // FIXME: it may be that prepend_current_dir_if_relative works OK on windows too, test!
        new_uri = g_strdup(filename);
#endif

        new_base = g_path_get_dirname(new_uri);
        new_name = g_path_get_basename(new_uri);
    } else {
        new_uri = g_strdup_printf(_("Unnamed document %d"), ++doc_count);
        new_base = NULL;
        new_name = g_strdup(this->uri);
    }

    // Update saveable repr attributes.
    Inkscape::XML::Node *repr = getReprRoot();

    // Changing uri in the document repr must not be not undoable.
    bool const saved = DocumentUndo::getUndoSensitive(this);
    DocumentUndo::setUndoSensitive(this, false);

    if (rebase) {
        Inkscape::XML::rebase_hrefs(this, new_base, true);
    }

    if (strncmp(new_name, "ink_ext_XXXXXX", 14))	// do not use temporary filenames
        repr->setAttribute("sodipodi:docname", new_name);
    DocumentUndo::setUndoSensitive(this, saved);


    g_free(this->name);
    g_free(this->base);
    g_free(this->uri);
    this->name = new_name;
    this->base = new_base;
    this->uri = new_uri;

    this->priv->uri_set_signal.emit(this->uri);
}

/**
 * Sets base, name and uri members of \a document.  Doesn't update
 * any relative hrefs in the document: thus, this is primarily for
 * newly-created documents.
 *
 * \see sp_document_change_uri_and_hrefs
 */
void SPDocument::setUri(gchar const *filename)
{
    do_change_uri(filename, false);
}

/**
 * Changes the base, name and uri members of \a document, and updates any
 * relative hrefs in the document to be relative to the new base.
 *
 * \see sp_document_set_uri
 */
void SPDocument::changeUriAndHrefs(gchar const *filename)
{
    do_change_uri(filename, true);
}

void SPDocument::emitResizedSignal(gdouble width, gdouble height)
{
    this->priv->resized_signal.emit(width, height);
}

sigc::connection SPDocument::connectModified(SPDocument::ModifiedSignal::slot_type slot)
{
    return priv->modified_signal.connect(slot);
}

sigc::connection SPDocument::connectURISet(SPDocument::URISetSignal::slot_type slot)
{
    return priv->uri_set_signal.connect(slot);
}

sigc::connection SPDocument::connectResized(SPDocument::ResizedSignal::slot_type slot)
{
    return priv->resized_signal.connect(slot);
}

sigc::connection
SPDocument::connectReconstructionStart(SPDocument::ReconstructionStart::slot_type slot)
{
    return priv->_reconstruction_start_signal.connect(slot);
}

void
SPDocument::emitReconstructionStart(void)
{
    // printf("Starting Reconstruction\n");
    priv->_reconstruction_start_signal.emit();
    return;
}

sigc::connection
SPDocument::connectReconstructionFinish(SPDocument::ReconstructionFinish::slot_type  slot)
{
    return priv->_reconstruction_finish_signal.connect(slot);
}

void
SPDocument::emitReconstructionFinish(void)
{
    // printf("Finishing Reconstruction\n");
    priv->_reconstruction_finish_signal.emit();

/**    
    // Reference to the old persp3d object is invalid after reconstruction.
    initialize_current_persp3d();
    
    return;
**/
}

sigc::connection SPDocument::connectCommit(SPDocument::CommitSignal::slot_type slot)
{
    return priv->commit_signal.connect(slot);
}



void SPDocument::_emitModified() {
    static guint const flags = SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_CHILD_MODIFIED_FLAG | SP_OBJECT_PARENT_MODIFIED_FLAG;
    root->emitModified(0);
    priv->modified_signal.emit(flags);
}

void SPDocument::bindObjectToId(gchar const *id, SPObject *object) {
    GQuark idq = g_quark_from_string(id);

    if (object) {
        g_assert(g_hash_table_lookup(priv->iddef, GINT_TO_POINTER(idq)) == NULL);
        g_hash_table_insert(priv->iddef, GINT_TO_POINTER(idq), object);
    } else {
        g_assert(g_hash_table_lookup(priv->iddef, GINT_TO_POINTER(idq)) != NULL);
        g_hash_table_remove(priv->iddef, GINT_TO_POINTER(idq));
    }

    SPDocumentPrivate::IDChangedSignalMap::iterator pos;

    pos = priv->id_changed_signals.find(idq);
    if ( pos != priv->id_changed_signals.end() ) {
        if (!(*pos).second.empty()) {
            (*pos).second.emit(object);
        } else { // discard unused signal
            priv->id_changed_signals.erase(pos);
        }
    }
}

void
SPDocument::addUndoObserver(Inkscape::UndoStackObserver& observer)
{
    this->priv->undoStackObservers.add(observer);
}

void
SPDocument::removeUndoObserver(Inkscape::UndoStackObserver& observer)
{
    this->priv->undoStackObservers.remove(observer);
}

SPObject *SPDocument::getObjectById(Glib::ustring const &id) const
{
    return getObjectById( id.c_str() );
}

SPObject *SPDocument::getObjectById(gchar const *id) const
{
    g_return_val_if_fail(id != NULL, NULL);
    if (!priv || !priv->iddef) {
    	return NULL;
    }

    GQuark idq = g_quark_from_string(id);
    gpointer rv = g_hash_table_lookup(priv->iddef, GINT_TO_POINTER(idq));
    if(rv != NULL)
    {
        return static_cast<SPObject*>(rv);
    }
    else
    {
        return NULL;
    }
}

sigc::connection SPDocument::connectIdChanged(gchar const *id,
                                              SPDocument::IDChangedSignal::slot_type slot)
{
    return priv->id_changed_signals[g_quark_from_string(id)].connect(slot);
}

void SPDocument::bindObjectToRepr(Inkscape::XML::Node *repr, SPObject *object)
{
    if (object) {
        g_assert(g_hash_table_lookup(priv->reprdef, repr) == NULL);
        g_hash_table_insert(priv->reprdef, repr, object);
    } else {
        g_assert(g_hash_table_lookup(priv->reprdef, repr) != NULL);
        g_hash_table_remove(priv->reprdef, repr);
    }
}

SPObject *SPDocument::getObjectByRepr(Inkscape::XML::Node *repr) const
{
    g_return_val_if_fail(repr != NULL, NULL);
    return static_cast<SPObject*>(g_hash_table_lookup(priv->reprdef, repr));
}

Glib::ustring SPDocument::getLanguage() const
{
    gchar const *document_language = rdf_get_work_entity(this, rdf_find_entity("language"));
    if (document_language) {
        while (isspace(*document_language))
            document_language++;
    }
    if ( !document_language || 0 == *document_language) {
        // retrieve system language
        document_language = getenv("LC_ALL");
        if ( NULL == document_language || *document_language == 0 ) {
            document_language = getenv ("LC_MESSAGES");
        }
        if ( NULL == document_language || *document_language == 0 ) {
            document_language = getenv ("LANG");
        }

        if ( NULL != document_language ) {
            const char *pos = strchr(document_language, '_');
            if ( NULL != pos ) {
                return Glib::ustring(document_language, pos - document_language);
            }
        }
    }

    if ( NULL == document_language )
        return Glib::ustring();
    return document_language;
}

/* Object modification root handler */

void SPDocument::requestModified()
{
    if (!modified_id) {
        modified_id = g_idle_add_full(SP_DOCUMENT_UPDATE_PRIORITY, 
                sp_document_idle_handler, this, NULL);
    }
    if (!rerouting_handler_id) {
        rerouting_handler_id = g_idle_add_full(SP_DOCUMENT_REROUTING_PRIORITY, 
                sp_document_rerouting_handler, this, NULL);
    }
}

void SPDocument::setupViewport(SPItemCtx *ctx)
{
    ctx->ctx.flags = 0;
    ctx->i2doc = Geom::identity();
    // Set up viewport in case svg has it defined as percentages
    if (root->viewBox_set) { // if set, take from viewBox
        ctx->viewport = root->viewBox;
    } else { // as a last resort, set size to A4
        ctx->viewport = Geom::Rect::from_xywh(0, 0, 210 * PX_PER_MM, 297 * PX_PER_MM);
    }
    ctx->i2vp = Geom::identity();
}

/**
 * Tries to update the document state based on the modified and
 * "update required" flags, and return true if the document has
 * been brought fully up to date.
 */
bool
SPDocument::_updateDocument()
{
    /* Process updates */
    if (this->root->uflags || this->root->mflags) {
        if (this->root->uflags) {
            SPItemCtx ctx;
            setupViewport(&ctx);

            bool saved = DocumentUndo::getUndoSensitive(this);
            DocumentUndo::setUndoSensitive(this, false);

            this->root->updateDisplay((SPCtx *)&ctx, 0);

            DocumentUndo::setUndoSensitive(this, saved);
        }
        this->_emitModified();
    }

    return !(this->root->uflags || this->root->mflags);
}


/**
 * Repeatedly works on getting the document updated, since sometimes
 * it takes more than one pass to get the document updated.  But it
 * usually should not take more than a few loops, and certainly never
 * more than 32 iterations.  So we bail out if we hit 32 iterations,
 * since this typically indicates we're stuck in an update loop.
 */
gint SPDocument::ensureUpToDate()
{
    // Bring the document up-to-date, specifically via the following:
    //   1a) Process all document updates.
    //   1b) When completed, process connector routing changes.
    //   2a) Process any updates resulting from connector reroutings.
    int counter = 32;
    for (unsigned int pass = 1; pass <= 2; ++pass) {
        // Process document updates.
        while (!_updateDocument()) {
            if (counter == 0) {
                g_warning("More than 32 iteration while updating document '%s'", uri);
                break;
            }
            counter--;
        }
        if (counter == 0)
        {
            break;
        }

        // After updates on the first pass we get libavoid to process all the 
        // changed objects and provide new routings.  This may cause some objects
            // to be modified, hence the second update pass.
        if (pass == 1) {
            router->processTransaction();
        }
    }
    
    if (modified_id) {
        // Remove handler
        g_source_remove(modified_id);
        modified_id = 0;
    }
    if (rerouting_handler_id) {
        // Remove handler
        g_source_remove(rerouting_handler_id);
        rerouting_handler_id = 0;
    }
    return counter>0;
}

/**
 * An idle handler to update the document.  Returns true if
 * the document needs further updates.
 */
static gint
sp_document_idle_handler(gpointer data)
{
    SPDocument *doc = static_cast<SPDocument *>(data);
    if (doc->_updateDocument()) {
        doc->modified_id = 0;
        return false;
    } else {
        return true;
    }
}

/**
 * An idle handler to reroute connectors in the document.  
 */
static gint
sp_document_rerouting_handler(gpointer data)
{
    // Process any queued movement actions and determine new routings for 
    // object-avoiding connectors.  Callbacks will be used to update and 
    // redraw affected connectors.
    SPDocument *doc = static_cast<SPDocument *>(data);
    doc->router->processTransaction();
    
    // We don't need to handle rerouting again until there are further 
    // diagram updates.
    doc->rerouting_handler_id = 0;
    return false;
}

static bool is_within(Geom::Rect const &area, Geom::Rect const &box)
{
    return area.contains(box);
}

static bool overlaps(Geom::Rect const &area, Geom::Rect const &box)
{
    return area.intersects(box);
}

static GSList *find_items_in_area(GSList *s, SPGroup *group, unsigned int dkey, Geom::Rect const &area,
                                  bool (*test)(Geom::Rect const &, Geom::Rect const &), bool take_insensitive = false)
{
    g_return_val_if_fail(SP_IS_GROUP(group), s);

    for ( SPObject *o = group->firstChild() ; o ; o = o->getNext() ) {
        if ( SP_IS_ITEM(o) ) {
            if (SP_IS_GROUP(o) && SP_GROUP(o)->effectiveLayerMode(dkey) == SPGroup::LAYER ) {
                s = find_items_in_area(s, SP_GROUP(o), dkey, area, test);
            } else {
                SPItem *child = SP_ITEM(o);
                Geom::OptRect box = child->desktopVisualBounds();
                if ( box && test(area, *box) && (take_insensitive || child->isVisibleAndUnlocked(dkey))) {
                    s = g_slist_append(s, child);
                }
            }
        }
    }

    return s;
}

/**
Returns true if an item is among the descendants of group (recursively).
 */
static bool item_is_in_group(SPItem *item, SPGroup *group)
{
    bool inGroup = false;
    for ( SPObject *o = group->firstChild() ; o && !inGroup; o = o->getNext() ) {
        if ( SP_IS_ITEM(o) ) {
            if (SP_ITEM(o) == item) {
                inGroup = true;
            } else if ( SP_IS_GROUP(o) ) {
                inGroup = item_is_in_group(item, SP_GROUP(o));
            }
        }
    }
    return inGroup;
}

SPItem *SPDocument::getItemFromListAtPointBottom(unsigned int dkey, SPGroup *group, GSList const *list,Geom::Point const p, bool take_insensitive)
{
    g_return_val_if_fail(group, NULL);
    SPItem *bottomMost = 0;

    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    gdouble delta = prefs->getDouble("/options/cursortolerance/value", 1.0);

    for ( SPObject *o = group->firstChild() ; o && !bottomMost; o = o->getNext() ) {
        if ( SP_IS_ITEM(o) ) {
            SPItem *item = SP_ITEM(o);
            Inkscape::DrawingItem *arenaitem = item->get_arenaitem(dkey);
            if (arenaitem && arenaitem->pick(p, delta, 1) != NULL
                && (take_insensitive || item->isVisibleAndUnlocked(dkey))) {
                if (g_slist_find((GSList *) list, item) != NULL) {
                    bottomMost = item;
                }
            }

            if ( !bottomMost && SP_IS_GROUP(o) ) {
                // return null if not found:
                bottomMost = getItemFromListAtPointBottom(dkey, SP_GROUP(o), list, p, take_insensitive);
            }
        }
    }
    return bottomMost;
}

/**
Returns the topmost (in z-order) item from the descendants of group (recursively) which
is at the point p, or NULL if none. Honors into_groups on whether to recurse into
non-layer groups or not. Honors take_insensitive on whether to return insensitive
items. If upto != NULL, then if item upto is encountered (at any level), stops searching
upwards in z-order and returns what it has found so far (i.e. the found item is
guaranteed to be lower than upto).
 */
static SPItem *find_item_at_point(unsigned int dkey, SPGroup *group, Geom::Point const p, gboolean into_groups, bool take_insensitive = false, SPItem *upto = NULL)
{
    SPItem *seen = NULL;
    SPItem *newseen = NULL;
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    gdouble delta = prefs->getDouble("/options/cursortolerance/value", 1.0);

    for ( SPObject *o = group->firstChild() ; o ; o = o->getNext() ) {
        if (!SP_IS_ITEM(o)) {
            continue;
        }

        if (upto && SP_ITEM(o) == upto) {
            break;
        }

        if (SP_IS_GROUP(o) && (SP_GROUP(o)->effectiveLayerMode(dkey) == SPGroup::LAYER || into_groups)) {
            // if nothing found yet, recurse into the group
            newseen = find_item_at_point(dkey, SP_GROUP(o), p, into_groups, take_insensitive, upto);
            if (newseen) {
                seen = newseen;
                newseen = NULL;
            }

            if (item_is_in_group(upto, SP_GROUP(o))) {
                break;
            }
        } else {
            SPItem *child = SP_ITEM(o);
            Inkscape::DrawingItem *arenaitem = child->get_arenaitem(dkey);

            // seen remembers the last (topmost) of items pickable at this point
            if (arenaitem && arenaitem->pick(p, delta, 1) != NULL
                && (take_insensitive || child->isVisibleAndUnlocked(dkey))) {
                seen = child;
            }
        }
    }
    return seen;
}

/**
Returns the topmost non-layer group from the descendants of group which is at point
p, or NULL if none. Recurses into layers but not into groups.
 */
static SPItem *find_group_at_point(unsigned int dkey, SPGroup *group, Geom::Point const p)
{
    SPItem *seen = NULL;
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();
    gdouble delta = prefs->getDouble("/options/cursortolerance/value", 1.0);

    for ( SPObject *o = group->firstChild() ; o ; o = o->getNext() ) {
        if (!SP_IS_ITEM(o)) {
            continue;
        }
        if (SP_IS_GROUP(o) && SP_GROUP(o)->effectiveLayerMode(dkey) == SPGroup::LAYER) {
            SPItem *newseen = find_group_at_point(dkey, SP_GROUP(o), p);
            if (newseen) {
                seen = newseen;
            }
        }
        if (SP_IS_GROUP(o) && SP_GROUP(o)->effectiveLayerMode(dkey) != SPGroup::LAYER ) {
            SPItem *child = SP_ITEM(o);
            Inkscape::DrawingItem *arenaitem = child->get_arenaitem(dkey);

            // seen remembers the last (topmost) of groups pickable at this point
            if (arenaitem && arenaitem->pick(p, delta, 1) != NULL) {
                seen = child;
            }
        }
    }
    return seen;
}

/*
 * Return list of items, contained in box
 *
 * Assumes box is normalized (and g_asserts it!)
 *
 */
GSList *SPDocument::getItemsInBox(unsigned int dkey, Geom::Rect const &box) const
{
    g_return_val_if_fail(this->priv != NULL, NULL);

    return find_items_in_area(NULL, SP_GROUP(this->root), dkey, box, is_within);
}

/*
 * Return list of items, that the parts of the item contained in box
 *
 * Assumes box is normalized (and g_asserts it!)
 *
 */

GSList *SPDocument::getItemsPartiallyInBox(unsigned int dkey, Geom::Rect const &box) const
{
    g_return_val_if_fail(this->priv != NULL, NULL);

    return find_items_in_area(NULL, SP_GROUP(this->root), dkey, box, overlaps);
}

GSList *SPDocument::getItemsAtPoints(unsigned const key, std::vector<Geom::Point> points) const
{
    GSList *items = NULL;
    Inkscape::Preferences *prefs = Inkscape::Preferences::get();

    // When picking along the path, we don't want small objects close together
    // (such as hatching strokes) to obscure each other by their deltas,
    // so we temporarily set delta to a small value
    gdouble saved_delta = prefs->getDouble("/options/cursortolerance/value", 1.0);
    prefs->setDouble("/options/cursortolerance/value", 0.25);

    for(unsigned int i = 0; i < points.size(); i++) {
        SPItem *item = getItemAtPoint(key, points[i],
                                                 false, NULL);
        if (item && !g_slist_find(items, item))
            items = g_slist_prepend (items, item);
    }

    // and now we restore it back
    prefs->setDouble("/options/cursortolerance/value", saved_delta);

    return items;
}

SPItem *SPDocument::getItemAtPoint( unsigned const key, Geom::Point const p,
                                    gboolean const into_groups, SPItem *upto) const
{
    g_return_val_if_fail(this->priv != NULL, NULL);

    return find_item_at_point(key, SP_GROUP(this->root), p, into_groups, false, upto);
}

SPItem *SPDocument::getGroupAtPoint(unsigned int key, Geom::Point const p) const
{
    g_return_val_if_fail(this->priv != NULL, NULL);

    return find_group_at_point(key, SP_GROUP(this->root), p);
}


// Resource management

bool SPDocument::addResource(gchar const *key, SPObject *object)
{
    g_return_val_if_fail(key != NULL, false);
    g_return_val_if_fail(*key != '\0', false);
    g_return_val_if_fail(object != NULL, false);
    g_return_val_if_fail(SP_IS_OBJECT(object), false);

    bool result = false;

    if ( !object->cloned ) {
        GSList *rlist = (GSList*)g_hash_table_lookup(priv->resources, key);
        g_return_val_if_fail(!g_slist_find(rlist, object), false);
        rlist = g_slist_prepend(rlist, object);
        g_hash_table_insert(priv->resources, (gpointer) key, rlist);

        GQuark q = g_quark_from_string(key);
        priv->resources_changed_signals[q].emit();

        result = true;
    }

    return result;
}

bool SPDocument::removeResource(gchar const *key, SPObject *object)
{
    g_return_val_if_fail(key != NULL, false);
    g_return_val_if_fail(*key != '\0', false);
    g_return_val_if_fail(object != NULL, false);
    g_return_val_if_fail(SP_IS_OBJECT(object), false);

    bool result = false;

    if ( !object->cloned ) {
        GSList *rlist = (GSList*)g_hash_table_lookup(priv->resources, key);
        g_return_val_if_fail(rlist != NULL, false);
        g_return_val_if_fail(g_slist_find(rlist, object), false);
        rlist = g_slist_remove(rlist, object);
        g_hash_table_insert(priv->resources, (gpointer) key, rlist);

        GQuark q = g_quark_from_string(key);
        priv->resources_changed_signals[q].emit();

        result = true;
    }

    return result;
}

GSList const *SPDocument::getResourceList(gchar const *key) const
{
    g_return_val_if_fail(key != NULL, NULL);
    g_return_val_if_fail(*key != '\0', NULL);

    return (GSList*)g_hash_table_lookup(this->priv->resources, key);
}

sigc::connection SPDocument::connectResourcesChanged(gchar const *key,
                                                     SPDocument::ResourcesChangedSignal::slot_type slot)
{
    GQuark q = g_quark_from_string(key);
    return this->priv->resources_changed_signals[q].connect(slot);
}

/* Helpers */

gboolean
sp_document_resource_list_free(gpointer /*key*/, gpointer value, gpointer /*data*/)
{
    g_slist_free((GSList *) value);
    return TRUE;
}

static unsigned int count_objects_recursive(SPObject *obj, unsigned int count)
{
    count++; // obj itself

    for ( SPObject *i = obj->firstChild(); i; i = i->getNext() ) {
        count = count_objects_recursive(i, count);
    }

    return count;
}

static unsigned int objects_in_document(SPDocument *document)
{
    return count_objects_recursive(document->getRoot(), 0);
}

static void vacuum_document_recursive(SPObject *obj)
{
    if (SP_IS_DEFS(obj)) {
        for ( SPObject *def = obj->firstChild(); def; def = def->getNext()) {
            // fixme: some inkscape-internal nodes in the future might not be collectable
            def->requestOrphanCollection();
        }
    } else {
        for ( SPObject *i = obj->firstChild(); i; i = i->getNext() ) {
            vacuum_document_recursive(i);
        }
    }
}

unsigned int SPDocument::vacuumDocument()
{
    unsigned int start = objects_in_document(this);
    unsigned int end = start;
    unsigned int newend = start;

    unsigned int iterations = 0;

    do {
        end = newend;

        vacuum_document_recursive(root);
        this->collectOrphans();
        iterations++;

        newend = objects_in_document(this);

    } while (iterations < 100 && newend < end);

    return start - newend;
}

bool SPDocument::isSeeking() const {
    return priv->seeking;
}

void SPDocument::setModifiedSinceSave(bool modified) {
    this->modified_since_save = modified;
    Gtk::Window *parent = SP_ACTIVE_DESKTOP->getToplevel();
    g_assert(parent != NULL);
    SPDesktopWidget *dtw = static_cast<SPDesktopWidget *>(parent->get_data("desktopwidget"));
    dtw->updateTitle( this->getName() );
}


/**
 * Paste SVG defs from the document retrieved from the clipboard into the active document.
 * @param clipdoc The document to paste.
 * @pre @c clipdoc != NULL and pasting into the active document is possible.
 */
void SPDocument::importDefs(SPDocument *source)
{
    Inkscape::XML::Node *root = source->getReprRoot();
    Inkscape::XML::Node *defs = sp_repr_lookup_name(root, "svg:defs", 1);
    Inkscape::XML::Node *target_defs = this->getDefs()->getRepr();

    prevent_id_clashes(source, this);

    for (Inkscape::XML::Node *def = defs->firstChild() ; def ; def = def->next()) {

        gboolean duplicate = false;
        SPObject *src = source->getObjectByRepr(def);

        // Prevent duplicates of solid swatches by checking if equivalent swatch already exists
        if (src && SP_IS_GRADIENT(src)) {
            SPGradient *gr = SP_GRADIENT(src);
            if (gr->isSolid() || gr->getVector()->isSolid()) {
                for (SPObject *trg = this->getDefs()->firstChild() ; trg ; trg = trg->getNext()) {
                    if (trg && SP_IS_GRADIENT(trg) && src != trg) {
                        if (gr->isEquivalent(SP_GRADIENT(trg))) {
                            // Change object references to the existing equivalent gradient
                            change_def_references(src, trg);
                            duplicate = true;
                            break;
                        }
                    }
                }
            }
        }

        // Prevent duplication of symbols... could be more clever.
        // The tag "_inkscape_duplicate" is added to "id" by ClipboardManagerImpl::copySymbol(). 
        // We assume that symbols are in defs section (not required by SVG spec).
        if (src && SP_IS_SYMBOL(src)) {

            Glib::ustring id = src->getRepr()->attribute("id");
            size_t pos = id.find( "_inkscape_duplicate" );
            if( pos != Glib::ustring::npos ) {

                // This is our symbol, now get rid of tag
                id.erase( pos ); 

                // Check that it really is a duplicate
                for (SPObject *trg = this->getDefs()->firstChild() ; trg ; trg = trg->getNext()) {
                    if( trg && SP_IS_SYMBOL(trg) && src != trg ) { 
                        std::string id2 = trg->getRepr()->attribute("id");

                        if( !id.compare( id2 ) ) {
                            duplicate = true;
                            break;
                        }
                    }
                }
                if ( !duplicate ) {
                    src->getRepr()->setAttribute("id", id.c_str() );
                }
            }
        }

        if (!duplicate) {
            Inkscape::XML::Node * dup = def->duplicate(this->getReprDoc());
            target_defs->appendChild(dup);
            Inkscape::GC::release(dup);
        }
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
