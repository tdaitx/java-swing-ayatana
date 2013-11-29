/*
 * Copyright (c) 2013 Jared González
 *
 * Permission is hereby granted, free of charge, to any
 * person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the
 * Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice
 * shall be included in all copies or substantial portions of
 * the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
 * KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
 * OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * File:   ayatana_Collections.c
 * Author: Jared González
 */
#include "com_jarego_jayatana_basic_GlobalMenu.h"

#include <jawt_md.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <gio/gio.h>
#include <libdbusmenu-glib/server.h>
#include <libdbusmenu-glib/client.h>
#include "com_jarego_jayatana_jni.h"
#include "com_jarego_jayatana_collections.h"
#include "com_jarego_jayatana_jkey2xkey.h"

/**
 * Estructura de control de menu global
 */
typedef struct {
	jlong windowXID;
	jobject globalThat;

	gchar *windowXIDPath;
	gboolean gdBusProxyRegistered;
	guint gBusWatcher;

	DbusmenuServer *dbusMenuServer;
	DbusmenuMenuitem *dbusMenuRoot;
	DbusmenuMenuitem *dbusMenuCurrent;
} jayatana_globalmenu_window;

/**
 * Generar nueva instancia de jayatana_globalmenu_window
 */
#define jayatana_globalmenu_window_new \
		(jayatana_globalmenu_window *)malloc(sizeof(jayatana_globalmenu_window))
ListIndex *jayatana_globalmenu_windows;

/**
 * Inicializar estructuras para GlobalMenu
 */
JNIEXPORT void JNICALL Java_com_jarego_jayatana_basic_GlobalMenu_initialize
  (JNIEnv *env, jclass thatclass) {
	jayatana_globalmenu_windows = collection_list_index_new();
}
/**
 * Termina las estructuras para GlobalMenu
 */
JNIEXPORT void JNICALL Java_com_jarego_jayatana_basic_GlobalMenu_uninitialize
  (JNIEnv *env, jclass thatclass) {
	collection_list_index_destory(jayatana_globalmenu_windows);
}

/**
 * Obtener la ubicacion de la ventana
 */
gchar *jayatana_get_windowxid_path(long xid) {
	gchar *xid_path;
	xid_path = (gchar *)malloc(sizeof(gchar *)*50);
	sprintf(xid_path, "/com/canonical/menu/%lx", xid);
	return xid_path;
}
/**
 * Destruir todos los menus
 */
void jayatana_destroy_menuitem(gpointer data) {
	if (data != NULL) {
		g_list_free_full(
				dbusmenu_menuitem_take_children((DbusmenuMenuitem *) data),
				jayatana_destroy_menuitem);
		g_object_unref(G_OBJECT(data));
	}
}
/**
 * Configurar aceleradores sobre el menu
 */
void jayatana_set_menuitem_shortcut(DbusmenuMenuitem *item, jint modifiers, jint keycode) {
	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE_ARRAY);
	if ((modifiers & JK_SHIFT) == JK_SHIFT)
		g_variant_builder_add(&builder, "s", DBUSMENU_MENUITEM_SHORTCUT_SHIFT);
	if ((modifiers & JK_CTRL) == JK_CTRL)
		g_variant_builder_add(&builder, "s",
				DBUSMENU_MENUITEM_SHORTCUT_CONTROL);
	if ((modifiers & JK_ALT) == JK_ALT)
		g_variant_builder_add(&builder, "s", DBUSMENU_MENUITEM_SHORTCUT_ALT);
	const char *keystring = jkeycode_to_xkey(keycode);
	g_variant_builder_add(&builder, "s", keystring);
	GVariant *inside = g_variant_builder_end(&builder);
	g_variant_builder_init(&builder, G_VARIANT_TYPE_ARRAY);
	g_variant_builder_add_value(&builder, inside);
	GVariant *outsidevariant = g_variant_builder_end(&builder);
	dbusmenu_menuitem_property_set_variant(item,
			DBUSMENU_MENUITEM_PROP_SHORTCUT, outsidevariant);
}

/**
 * Obtener el identificar X de una ventana AWT
 */
JNIEXPORT jlong JNICALL Java_com_jarego_jayatana_basic_GlobalMenu_getWindowXID(
  JNIEnv *env, jclass thatclass, jobject window) {
	JAWT awt;
	JAWT_DrawingSurface* ds;
	JAWT_DrawingSurfaceInfo* dsi;
	JAWT_X11DrawingSurfaceInfo* dsi_x11;
	jint dsLock;
	Drawable drawable = -1l;
	awt.version = JAWT_VERSION_1_4;
	if (JAWT_GetAWT(env, &awt) != 0) {
		ds = awt.GetDrawingSurface(env, window);
		if (ds != NULL) {
			dsLock = ds->Lock(ds);
			if ((dsLock & JAWT_LOCK_ERROR) == 0) {
				dsi = ds->GetDrawingSurfaceInfo(ds);
				dsi_x11 = (JAWT_X11DrawingSurfaceInfo*) dsi->platformInfo;
				drawable = dsi_x11->drawable;
				ds->FreeDrawingSurfaceInfo(dsi);
				ds->Unlock(ds);
			}
		}
		awt.FreeDrawingSurface(ds);
	}
	return (long)drawable;
}

/**
 * Notificación de bus disponible para menu global
 */
void jayatana_on_registrar_available(
		GDBusConnection *connection, const gchar *name,
		const gchar *name_owner, gpointer user_data) {
	// recuperar el controlador
	jayatana_globalmenu_window *globalmenu_window = (jayatana_globalmenu_window *)user_data;
	if (!globalmenu_window->gdBusProxyRegistered) {
		// generar menus
		globalmenu_window->windowXIDPath = jayatana_get_windowxid_path(globalmenu_window->windowXID);
		globalmenu_window->dbusMenuServer = dbusmenu_server_new(globalmenu_window->windowXIDPath);
		globalmenu_window->dbusMenuRoot = dbusmenu_menuitem_new();
		globalmenu_window->dbusMenuCurrent = globalmenu_window->dbusMenuRoot;
		dbusmenu_server_set_root(globalmenu_window->dbusMenuServer,globalmenu_window->dbusMenuRoot);
		// registrar bus
		GDBusProxy *dbBusProxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
				G_DBUS_PROXY_FLAGS_NONE, NULL,
				"com.canonical.AppMenu.Registrar",
				"/com/canonical/AppMenu/Registrar",
				"com.canonical.AppMenu.Registrar",
				NULL, NULL);
		g_dbus_proxy_call_sync(dbBusProxy, "RegisterWindow",
				g_variant_new("(uo)", (guint32)globalmenu_window->windowXID,
						globalmenu_window->windowXIDPath), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
				NULL);
		// notificar a clase java la integración
		JNIEnv *env = NULL;
		(*jayatana_jvm)->AttachCurrentThread(jayatana_jvm, (void**)&env, NULL);
		jclass thatclass = (*env)->GetObjectClass(env, globalmenu_window->globalThat);
		jmethodID mid = (*env)->GetMethodID(env, thatclass, "register", "()V");
		(*env)->CallVoidMethod(env, globalmenu_window->globalThat, mid);
		(*jayatana_jvm)->DetachCurrentThread(jayatana_jvm);
		// marcar como instalado
		globalmenu_window->gdBusProxyRegistered = True;
	}
}

/**
 * Notificación de bus no disponible para menu global
 */
void jayatana_on_registrar_unavailable(
		GDBusConnection *connection, const gchar *name,
		gpointer user_data) {
	//recuperar el controlador
	jayatana_globalmenu_window *globalmenu_window = (jayatana_globalmenu_window *)user_data;
	if (globalmenu_window->gdBusProxyRegistered) {
		// liberar menus
		g_list_free_full(dbusmenu_menuitem_take_children(globalmenu_window->dbusMenuRoot),
				jayatana_destroy_menuitem);
		g_object_unref(globalmenu_window->dbusMenuRoot);
		g_object_unref(globalmenu_window->dbusMenuServer);
		// liberar ruta de ventana
		free(globalmenu_window->windowXIDPath);
		// notificar a java el deregistro
		JNIEnv *env = NULL;
		(*jayatana_jvm)->AttachCurrentThread(jayatana_jvm, (void**)&env, NULL);
		jclass thatclass = (*env)->GetObjectClass(env, globalmenu_window->globalThat);
		jmethodID mid = (*env)->GetMethodID(env, thatclass, "unregister", "()V");
		(*env)->CallVoidMethod(env, globalmenu_window->globalThat, mid);
		(*jayatana_jvm)->DetachCurrentThread(jayatana_jvm);
		// marcar como desinstaldo
		globalmenu_window->gdBusProxyRegistered = False;
	}
}

/**
 * Registrar un control de bus para menu global
 */
JNIEXPORT void JNICALL Java_com_jarego_jayatana_basic_GlobalMenu_registerWatcher
  (JNIEnv *env, jobject that, jlong windowXID) {
	//generar el controlador
	jayatana_globalmenu_window *globalmenu_window = jayatana_globalmenu_window_new;
	globalmenu_window->windowXID = windowXID;
	globalmenu_window->globalThat = (*env)->NewGlobalRef(env, that);
	globalmenu_window->gdBusProxyRegistered = False;
	collection_list_index_add(jayatana_globalmenu_windows, windowXID, globalmenu_window);
	// iniciar bus para menu global
	globalmenu_window->gBusWatcher = g_bus_watch_name(G_BUS_TYPE_SESSION,
			"com.canonical.AppMenu.Registrar", G_BUS_NAME_WATCHER_FLAGS_NONE,
			jayatana_on_registrar_available, jayatana_on_registrar_unavailable,
			globalmenu_window, NULL);

}

/**
 * Deregistrar un control de bus para menu global
 */
JNIEXPORT void JNICALL Java_com_jarego_jayatana_basic_GlobalMenu_unregisterWatcher
  (JNIEnv *env, jobject that, jlong windowXID) {
	//recuperar el controlador
	jayatana_globalmenu_window *globalmenu_window = (jayatana_globalmenu_window*)
			collection_list_index_remove(jayatana_globalmenu_windows, windowXID);
	if (globalmenu_window->gdBusProxyRegistered) {
		// liberar menus
		g_list_free_full(dbusmenu_menuitem_take_children(globalmenu_window->dbusMenuRoot),
				jayatana_destroy_menuitem);
		g_object_unref(globalmenu_window->dbusMenuRoot);
		g_object_unref(globalmenu_window->dbusMenuServer);
		// liberar ruta de ventana
		free(globalmenu_window->windowXIDPath);
		// notificar a clase java
		jclass thatclass = (*env)->GetObjectClass(env, that);
		jmethodID mid = (*env)->GetMethodID(env, thatclass, "unregister", "()V");
		(*env)->CallVoidMethod(env, that, mid);
	}
	(*env)->DeleteGlobalRef(env, globalmenu_window->globalThat);
	g_bus_unwatch_name(globalmenu_window->gBusWatcher);
	free(globalmenu_window);
}

void jayatana_item_event_open(DbusmenuMenuitem *item) {
	// recuperar el controlador
	jayatana_globalmenu_window *globalmenu_window = (jayatana_globalmenu_window*)
			collection_list_index_get(jayatana_globalmenu_windows,
					g_variant_get_int64(dbusmenu_menuitem_property_get_variant(
							item, "jayatana-windowxid")));
	// inicializar menu
	globalmenu_window->dbusMenuCurrent = item;
	g_list_free_full(dbusmenu_menuitem_take_children(item), jayatana_destroy_menuitem);
	// invocar generacion de menus
	JNIEnv *env = NULL;
	(*jayatana_jvm)->AttachCurrentThread(jayatana_jvm, (void**) &env, NULL);
	jclass thatclass = (*env)->GetObjectClass(env, globalmenu_window->globalThat);
	jmethodID mid = (*env)->GetMethodID(env, thatclass, "menuAboutToShow", "(I)V");
	(*env)->CallVoidMethod(env, globalmenu_window->globalThat, mid,
			dbusmenu_menuitem_property_get_int(item, "jayatana-menuid"));
	(*jayatana_jvm)->DetachCurrentThread(jayatana_jvm);
}

void jayatana_item_event_close(DbusmenuMenuitem *item, const char *event) {
	if (strcmp(DBUSMENU_MENUITEM_EVENT_CLOSED, event) == 0) {
		// recuperar el controlador
		jayatana_globalmenu_window *globalmenu_window = (jayatana_globalmenu_window*)
				collection_list_index_get(jayatana_globalmenu_windows,
						g_variant_get_int64(dbusmenu_menuitem_property_get_variant(
								item, "jayatana-windowxid")));
		// invocar cerrado de menu
		JNIEnv *env = NULL;
		(*jayatana_jvm)->AttachCurrentThread(jayatana_jvm, (void**) &env, NULL);
		jclass thatclass = (*env)->GetObjectClass(env, globalmenu_window->globalThat);
		jmethodID mid = (*env)->GetMethodID(env, thatclass, "menuAfterClose", "(I)V");
		(*env)->CallVoidMethod(env, globalmenu_window->globalThat, mid,
				dbusmenu_menuitem_property_get_int(item, "jayatana-menuid"));
		(*jayatana_jvm)->DetachCurrentThread(jayatana_jvm);

	}
}

void jayatana_item_activated(DbusmenuMenuitem *item, guint timestamp, gpointer user_data) {
	// recuperar el controlador
	jayatana_globalmenu_window *globalmenu_window = (jayatana_globalmenu_window*)
			collection_list_index_get(jayatana_globalmenu_windows,
					g_variant_get_int64(dbusmenu_menuitem_property_get_variant(
							item, "jayatana-windowxid")));
	//invocar hacia java
	JNIEnv *env = NULL;
	(*jayatana_jvm)->AttachCurrentThread(jayatana_jvm, (void**) &env, NULL);
	jclass thatclass = (*env)->GetObjectClass(env, globalmenu_window->globalThat);
	jmethodID mid = (*env)->GetMethodID(env, thatclass, "menuActivated", "(I)V");
	(*env)->CallVoidMethod(env, globalmenu_window->globalThat, mid,
			dbusmenu_menuitem_property_get_int(item, "jayatana-menuid"));
	(*jayatana_jvm)->DetachCurrentThread(jayatana_jvm);
}

JNIEXPORT void JNICALL Java_com_jarego_jayatana_basic_GlobalMenu_addMenu
  (JNIEnv *env, jobject that, jlong windowXID, jint menuID, jstring label, jboolean enabled) {
	// recuperar controlador
	jayatana_globalmenu_window *globalmenu_window = (jayatana_globalmenu_window*)
			collection_list_index_get(jayatana_globalmenu_windows, windowXID);
	// obtener etiqueta del menu
	const char *cclabel = (*env)->GetStringUTFChars(env, label, 0);
	// generar menu
	DbusmenuMenuitem *item = dbusmenu_menuitem_new();
	dbusmenu_menuitem_property_set(item, DBUSMENU_MENUITEM_PROP_LABEL, cclabel);
	dbusmenu_menuitem_property_set(item, DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY,
			DBUSMENU_MENUITEM_CHILD_DISPLAY_SUBMENU);
	dbusmenu_menuitem_property_set_int(item, "jayatana-menuid", menuID);
	dbusmenu_menuitem_property_set_variant(item, "jayatana-windowxid",
			g_variant_new_int64(globalmenu_window->windowXID));
	dbusmenu_menuitem_property_set_bool(item, DBUSMENU_MENUITEM_PROP_ENABLED, (gboolean)enabled);
	dbusmenu_menuitem_child_append(globalmenu_window->dbusMenuCurrent, item);
	g_signal_connect(G_OBJECT(item), DBUSMENU_MENUITEM_SIGNAL_EVENT,
			G_CALLBACK(jayatana_item_event_close), NULL);
	g_signal_connect(G_OBJECT(item), DBUSMENU_MENUITEM_SIGNAL_ABOUT_TO_SHOW,
			G_CALLBACK(jayatana_item_event_open), NULL);

	DbusmenuMenuitem *foo = dbusmenu_menuitem_new();
	dbusmenu_menuitem_property_set(foo, DBUSMENU_MENUITEM_PROP_LABEL, "");
	dbusmenu_menuitem_child_append(item, foo);
}

JNIEXPORT void JNICALL Java_com_jarego_jayatana_basic_GlobalMenu_addMenuItem
  (JNIEnv *env, jobject that, jlong windowXID, jint menuID, jstring label, jboolean enabled, jint modifiers, jint keycode) {
	// recuperar controlador
	jayatana_globalmenu_window *globalmenu_window = (jayatana_globalmenu_window*)
			collection_list_index_get(jayatana_globalmenu_windows, windowXID);
	// obtener etiqueta del menu
	const char *cclabel = (*env)->GetStringUTFChars(env, label, 0);
	// generar menu
	DbusmenuMenuitem *item = dbusmenu_menuitem_new();
	dbusmenu_menuitem_property_set(item, DBUSMENU_MENUITEM_PROP_LABEL, cclabel);
	dbusmenu_menuitem_property_set_bool(item, DBUSMENU_MENUITEM_PROP_ENABLED, (gboolean)enabled);
	dbusmenu_menuitem_property_set_int(item, "jayatana-menuid", menuID);
	dbusmenu_menuitem_property_set_variant(item, "jayatana-windowxid",
				g_variant_new_int64(globalmenu_window->windowXID));
	if (modifiers > -1 && keycode > -1)
		jayatana_set_menuitem_shortcut(item, modifiers, keycode);
	g_signal_connect(G_OBJECT(item), DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
			G_CALLBACK(jayatana_item_activated), NULL);

	dbusmenu_menuitem_child_append(globalmenu_window->dbusMenuCurrent, item);
}

JNIEXPORT void JNICALL Java_com_jarego_jayatana_basic_GlobalMenu_addMenuItemRadio
  (JNIEnv *env, jobject that, jlong windowXID, jint menuID, jstring label, jboolean enabled, jint modifiers, jint keycode, jboolean selected) {
	// recuperar controlador
	jayatana_globalmenu_window *globalmenu_window = (jayatana_globalmenu_window*)
			collection_list_index_get(jayatana_globalmenu_windows, windowXID);
	// obtener etiqueta del menu
	const char *cclabel = (*env)->GetStringUTFChars(env, label, 0);
	// generar menu
	DbusmenuMenuitem *item = dbusmenu_menuitem_new();
	dbusmenu_menuitem_property_set(item, DBUSMENU_MENUITEM_PROP_LABEL, cclabel);
	dbusmenu_menuitem_property_set_bool(item, DBUSMENU_MENUITEM_PROP_ENABLED, (gboolean)enabled);
	dbusmenu_menuitem_property_set_int(item, "jayatana-menuid", menuID);
	dbusmenu_menuitem_property_set_variant(item, "jayatana-windowxid",
				g_variant_new_int64(globalmenu_window->windowXID));
	if (modifiers > -1 && keycode > -1)
		jayatana_set_menuitem_shortcut(item, modifiers, keycode);
	dbusmenu_menuitem_property_set (item, DBUSMENU_MENUITEM_PROP_TOGGLE_TYPE,
			DBUSMENU_MENUITEM_TOGGLE_RADIO);
	dbusmenu_menuitem_property_set_int(item, DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
			selected ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
	g_signal_connect(G_OBJECT(item), DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
			G_CALLBACK(jayatana_item_activated), NULL);
	dbusmenu_menuitem_child_append(globalmenu_window->dbusMenuCurrent, item);
}

JNIEXPORT void JNICALL Java_com_jarego_jayatana_basic_GlobalMenu_addMenuItemCheck
  (JNIEnv *env, jobject that, jlong windowXID, jint menuID, jstring label, jboolean enabled, jint modifiers, jint keycode, jboolean selected) {
	// recuperar controlador
	jayatana_globalmenu_window *globalmenu_window = (jayatana_globalmenu_window*)
			collection_list_index_get(jayatana_globalmenu_windows, windowXID);
	// obtener etiqueta del menu
	const char *cclabel = (*env)->GetStringUTFChars(env, label, 0);
	// generar menu
	DbusmenuMenuitem *item = dbusmenu_menuitem_new();
	dbusmenu_menuitem_property_set(item, DBUSMENU_MENUITEM_PROP_LABEL, cclabel);
	dbusmenu_menuitem_property_set_bool(item, DBUSMENU_MENUITEM_PROP_ENABLED, (gboolean)enabled);
	dbusmenu_menuitem_property_set_int(item, "jayatana-menuid", menuID);
	dbusmenu_menuitem_property_set_variant(item, "jayatana-windowxid",
				g_variant_new_int64(globalmenu_window->windowXID));
	if (modifiers > -1 && keycode > -1)
		jayatana_set_menuitem_shortcut(item, modifiers, keycode);
	dbusmenu_menuitem_property_set (item, DBUSMENU_MENUITEM_PROP_TOGGLE_TYPE,
			DBUSMENU_MENUITEM_TOGGLE_CHECK);
	dbusmenu_menuitem_property_set_int(item, DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
			selected ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
	g_signal_connect(G_OBJECT(item), DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
			G_CALLBACK(jayatana_item_activated), NULL);
	dbusmenu_menuitem_child_append(globalmenu_window->dbusMenuCurrent, item);
}

JNIEXPORT void JNICALL Java_com_jarego_jayatana_basic_GlobalMenu_addSeparator
  (JNIEnv *env, jobject that, jlong windowXID) {
	// recuperar controlador
	jayatana_globalmenu_window *globalmenu_window = (jayatana_globalmenu_window*)
			collection_list_index_get(jayatana_globalmenu_windows, windowXID);
	// generar separador
	DbusmenuMenuitem *item = dbusmenu_menuitem_new();
	dbusmenu_menuitem_property_set(item, DBUSMENU_MENUITEM_PROP_TYPE, DBUSMENU_CLIENT_TYPES_SEPARATOR);
	dbusmenu_menuitem_child_append(globalmenu_window->dbusMenuCurrent, item);
}
