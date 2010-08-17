/*******************************************************************************
  Copyright (c) 2009-2010 Dmitry Matveev <dmatveev@inbox.com>

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*******************************************************************************/

#include <gtk/gtk.h>
#include <glib/gprintf.h>
#include <stdlib.h>
#include <string.h>

#ifdef G_OS_UNIX
  #include <errno.h>
  #include <signal.h>
  #include <unistd.h>
  #include <sys/socket.h>
  #include <sys/ioctl.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <fcntl.h>
#elif defined(G_OS_WIN32)
  #include <winsock2.h>
  #define EINPROGRESS WSAEINPROGRESS    /* stupid Windows sockets */
#endif

#define MARGIN 7
#define ever (;;)

#define STYLE_IN    "packet-in"
#define STYLE_OUT   "packet-out"
#define STYLE_MONO  "mono"

struct wsess {
    GtkWidget *window;
    GtkWidget *dump;
    GtkWidget *entry;
    GtkTextBuffer *buffer;
    GtkWidget *send;
};

struct session {
    int sock;
    guint rwatch;
    guint dwatch;
    GIOChannel *chan;
    struct wsess *ws;
    struct sockaddr_in sa;
    char cond;              /* 1: online, 0: offline */
};

enum direction {
    dcn_in = 0,
    dcn_out,
};

enum condition {
    cond_on = 0,
    cond_off,
};

/* global variables are evil */
static int    g_sock;
static GList *g_client_channels;
static GList *g_client_sockets;

#define fatal(X) \
    gtk_err (X); 

static GtkWidget *create_setup();
static struct wsess *create_wsession();
static void start_server_cb (GtkDialog *dlg, gint resp, gpointer ud);
void server_thread (int port);
static gboolean receive_cb (GIOChannel *src, GIOCondition cond, gpointer ud);
static gboolean disconnected_cb (GIOChannel *src, GIOCondition cond, gpointer ud);
static void send_cb (GtkButton *btn, gpointer ud);
static gboolean close_cb (GtkWidget *widget, GdkEvent *event, gpointer ud);
static long bytes_available (int sock);
static void gtk_buffer_dump ( struct session *ses, unsigned char *packet,
                              int length, enum direction dcn);
static void gtk_err (const gchar *text);

static void gtk_err (const gchar *text) {
    GtkWidget *dialog;

    dialog = gtk_message_dialog_new (NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                     GTK_BUTTONS_OK,
                                     "Operation failed:");

    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s",
                                              text);
    gtk_window_set_title (GTK_WINDOW (dialog), "cm");
    gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);
}

static gboolean disconnected_cb (GIOChannel *src, GIOCondition cond, gpointer ud) {
    struct session *ses = (struct session *) ud;

    if (!src || !ud || ses->cond == cond_off)
        return FALSE;

    g_print ("somebody disconnected from us...\n");

    g_io_channel_shutdown (ses->chan, FALSE, 0);
    gtk_window_set_title (GTK_WINDOW (ses->ws->window), "[remote host down]");
    gtk_text_view_set_editable (GTK_TEXT_VIEW (ses->ws->entry), FALSE);
    ses->cond = cond_off;
    
    return FALSE;
}

static gboolean close_cb (GtkWidget *widget, GdkEvent *event, gpointer ud) {
    struct session *ses = (struct session *) ud;
    
    if (!widget || !ses)
        return FALSE;

    if (ses->cond != cond_off) {
        ses->cond = cond_off;
        g_io_channel_shutdown (ses->chan, FALSE, 0);
    }
    gtk_widget_destroy (ses->ws->window);
    free (ses);

    g_print ("connection with client was closed.\n");
    return FALSE;
}

static void gtk_buffer_dump (struct session *ses, unsigned char *packet,
                             int size, enum direction dcn) {
    int i, left;
    char byte[8];
    GString *dump, *ascii;
    GtkTextIter end;

    dump = g_string_sized_new (size * 3);
    ascii = g_string_sized_new (10);
    left = size;

    for (i = 0; i < size; ) {
        int j, k;

        for (k = 0; k < 2; k++ ) {
            int actual = (left > 8 ? 8 : left);

            for (j = 0; j < 8; j++) {
                if (j < actual) {
                    unsigned char b = packet[i+j];

                    g_sprintf (byte, "%02X ", b);
                    g_string_append (dump, byte);
                    g_string_append_c (ascii, (b >= 32 && b <= 128) ? b : '.');
                }
                else
                    g_string_append (dump, "   ");
            }

            i += j;
            left -= j;
            if (left && k == 0) {
                g_string_append (ascii, "  ");
                g_string_append (dump, "  ");
            }
        }
        g_string_append (dump, "    ");
        g_string_append (dump, ascii->str);
        g_string_append_c (dump, '\n');
        g_string_assign (ascii, "");
    }
    
    gtk_text_buffer_get_end_iter (ses->ws->buffer, &end);
    gtk_text_buffer_insert_with_tags_by_name
        (ses->ws->buffer, &end, dump->str, dump->len,
         dcn == dcn_in ? STYLE_IN : STYLE_OUT, STYLE_MONO, NULL );
    
    g_string_free (dump, TRUE);
    g_string_free (ascii, TRUE);
}


static long bytes_available (int sock) {
#ifdef G_OS_UNIX
    long int avail = 0;
    size_t bytes;
    
    if (ioctl (sock, FIONREAD, (char *) &bytes) >= 0)
        return avail = (long int) *((int *) &bytes);
    
#elif defined (G_OS_WIN32)
    unsigned long  bytes = 0;
    unsigned long dummy = 0;
    DWORD sizew = 0;
    
    if (::WSAIoctl (sock, FIONREAD, &dummy, sizeof (dummy), &bytes,
                   sizeof (bytes), &sizew, 0,0) == SOCKET_ERROR)
        return -1;
    return bytes;
#endif
    return 0;
}

int space_character (char c) {
    return (c == ' ' || c == '\t' || c == '\n');
}

static void send_cb (GtkButton *btn, gpointer ud) {
    struct session *ses = (struct session *) ud;
    GtkTextIter start, end;
    GtkTextBuffer *bfr;
    int i, size, actual = 0;
    unsigned char *buffer;
    char *text, *ptr, *endptr;
    
    if (!btn || !ud || ses->cond != cond_on)
        return;

    bfr = gtk_text_view_get_buffer (GTK_TEXT_VIEW (ses->ws->entry));
    gtk_text_buffer_get_bounds (bfr, &start, &end);
    text = ptr = gtk_text_buffer_get_text (bfr, &start, &end, FALSE );

    size = strlen (text);
    buffer = malloc (size);
    for (i = 0; i < size;) {
        long byte = strtol (ptr, &endptr, 16);
        if (ptr == endptr) {
            int j, spaces = 0;
            for (j = i; j < size ; j++) {
                if (!space_character (text [j])) {
                    gtk_err ("Parse error!");
                    g_free (text);
                    free (buffer);
                    return;
                }
                else
                    ++spaces;
            }
            if (spaces == size - i)
                break; /* line ending with white space */
        }

        buffer [actual++] = byte;

        if (*endptr == '\0') break;
        i += (endptr - ptr);
        ptr = endptr;
    }

    g_print ("%d bytes sent\n", send (ses->sock, buffer, actual, 0));
    gtk_buffer_dump (ses, buffer, actual, dcn_out);

    free (buffer);
    g_free (text);
    gtk_text_buffer_delete (bfr, &start, &end);
}

static gboolean receive_cb (GIOChannel *src, GIOCondition cond, gpointer ud) {
    struct session *ses;
    unsigned char *buffer;
    gchar title[256];
    long size;

    if (!src || !ud) {
        g_print ("some error in cb\n");
        return FALSE;
    }

    /* dump the received bytes in the session window */
    ses = (struct session *) ud;

    size = bytes_available (ses->sock);
    buffer = malloc (size);
    recv (ses->sock, buffer, size, MSG_WAITALL);

    gtk_buffer_dump (ses, buffer, size, dcn_in );
    g_sprintf (title, "TCP/IP Session with %s", inet_ntoa (ses->sa.sin_addr));
    gtk_window_set_title (GTK_WINDOW (ses->ws->window), title);
    gtk_widget_show (ses->ws->window);
    
    return FALSE;
}

void server_thread (int port) {
    struct sockaddr_in sa;

    g_print("Server wakes up on port %d\n", port);

    /* create socket and open port */
    g_sock = socket (PF_INET, SOCK_STREAM, 0);
    if (g_sock == -1) {
        printf ("OOps with code %d", errno);
        fflush (stdout);
        exit (1);
    }

    /* set SO_REUSEADDR to make application able to restart quickly */
    int on = 1;
    if (setsockopt (g_sock, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) == -1)
        fatal ("Failed to set SO_REUSEADDR socket option");

    int val;
    val = fcntl (g_sock, F_GETFL, 0);
    if (fcntl (g_sock, F_SETFL, val | O_NONBLOCK) == -1)
        fatal ("Failed to turn socket into non-blocking mode");
    
    memset (&sa, 0, sizeof (sa));
    sa.sin_family = PF_INET;
    sa.sin_port = htons (port);
    sa.sin_addr.s_addr = htonl (INADDR_ANY);

    if (bind (g_sock, (struct sockaddr *) &sa, sizeof (sa)) == -1)
        fatal ("Failed to bind socket");
    
    if (listen (g_sock, SOMAXCONN) != 0)
        fatal ("Failed to listen on socket");

    for ever {
        struct session *ses;
        socklen_t clsl;

        if (g_sock == 0)
            return; /* good night, sweet prince */
      
        ses  = calloc (1, sizeof (struct session));
        clsl = sizeof (ses->sa);

        ses->ws = create_wsession (ses);

        /* wait while client will connect */
        ses->sock = -1;
        while (g_sock != 0 && (ses->sock == EAGAIN || ses->sock < 0)) {
            ses->sock = accept (g_sock, (struct sockaddr *) &ses->sa, &clsl);

            while (gtk_events_pending ())
                gtk_main_iteration ();

            g_usleep (10000);
        }

        if (g_sock == 0)
            return; /* it could changed here after catching SIGINT */
      
#ifdef G_OS_UNIX
        ses->chan = g_io_channel_unix_new (ses->sock);
#elif defined (G_OS_WIN32)
        ses->chan = g_io_channel_win32_new_socket (ses->sock);
#else
        abort();
#endif
        g_client_channels = g_list_append (g_client_channels, (gpointer) ses->chan);
        g_client_sockets  = g_list_append (g_client_sockets, GINT_TO_POINTER (ses->sock));
        ses->cond = cond_on;

        ses->rwatch = g_io_add_watch (ses->chan, G_IO_IN | G_IO_PRI,
                                      receive_cb, ses);
        ses->dwatch = g_io_add_watch (ses->chan, G_IO_ERR | G_IO_HUP | G_IO_NVAL,
                                      disconnected_cb, ses);
        g_print ("new client has connected\n");
    }
    
    return;
}

static void start_server_cb (GtkDialog *dlg, gint resp, gpointer ud) {
    if (!dlg || !ud || resp == GTK_RESPONSE_REJECT)
        exit(0);

    if (resp == GTK_RESPONSE_ACCEPT) {
        GtkWidget *spin;
        int port;

        spin = (GtkWidget *) ud;
        port = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (spin));
        
        gtk_widget_destroy (GTK_WIDGET (dlg));
        server_thread (port);
    }
}

static GtkWidget *create_setup() {
    GtkWidget *dialog, *hbox, *label, *entry;
    
    dialog = gtk_dialog_new_with_buttons ("Start server", NULL, GTK_DIALOG_MODAL,
                                          GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
                                          GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
                                          NULL);

    hbox = gtk_hbox_new (FALSE, MARGIN);
    gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->vbox), hbox,
                        TRUE, TRUE, MARGIN);

    label = gtk_label_new ("Listen on port");
    gtk_box_pack_start (GTK_BOX(hbox), label, TRUE, TRUE, MARGIN);

    entry = gtk_spin_button_new_with_range (1, 65535, 1);
    gtk_box_pack_start (GTK_BOX(hbox), entry, TRUE, TRUE, MARGIN);

    g_signal_connect (dialog, "response", G_CALLBACK(start_server_cb), entry);
    gtk_widget_show_all (hbox);

    return dialog;
}

static struct wsess *create_wsession (struct session *ses) {
    GtkWidget *vbox, *sw, *hbox, *paned;
    struct wsess *ws = calloc (1, sizeof (struct wsess));
    
    ws->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size (GTK_WINDOW(ws->window), 550, 300);

    vbox = gtk_vbox_new (FALSE, MARGIN);
    gtk_container_add (GTK_CONTAINER (ws->window), vbox);

    paned = gtk_vpaned_new ();

    ws->dump = gtk_text_view_new ();
    gtk_text_view_set_editable (GTK_TEXT_VIEW (ws->dump), FALSE);

    sw = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(sw),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW(sw),
                                         GTK_SHADOW_ETCHED_IN);
    
    gtk_container_add (GTK_CONTAINER (sw), ws->dump);
    gtk_paned_pack1 (GTK_PANED (paned), sw, TRUE, TRUE);
    gtk_widget_set_size_request (sw, 300, 200);

    ws->entry = gtk_text_view_new ();
    sw = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(sw),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW(sw),
                                         GTK_SHADOW_ETCHED_IN);
    gtk_container_add (GTK_CONTAINER (sw), ws->entry);
    gtk_paned_pack2 (GTK_PANED(paned), sw, FALSE, TRUE);
    gtk_widget_set_size_request (sw, -1, 60);

    gtk_box_pack_start (GTK_BOX (vbox), paned, TRUE, TRUE, 0);
    
    ws->buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (ws->dump));

    gtk_text_buffer_create_tag (ws->buffer, STYLE_MONO, "family", "monospace",
                                NULL);
    
    gtk_text_buffer_create_tag (ws->buffer, STYLE_IN, "paragraph-background",
                                "lightblue", NULL);

    gtk_text_buffer_create_tag (ws->buffer, STYLE_OUT, "family", "monospace",
                                NULL);

    gtk_widget_modify_font (ws->entry,
                            pango_font_description_from_string ("monospace"));
    gtk_widget_grab_focus (ws->entry);
    
    hbox = gtk_hbox_new (FALSE, 0);
    ws->send = gtk_button_new_with_label ("Send");
    gtk_box_pack_end (GTK_BOX (hbox), ws->send, FALSE, FALSE, MARGIN);
    gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, MARGIN);

    g_signal_connect (ws->send, "clicked", G_CALLBACK(send_cb), ses);
    g_signal_connect (ws->window, "delete-event", G_CALLBACK(close_cb), ses);

    gtk_widget_show_all (vbox);
    
    return ws;
}

void catch_int (int segnum) {
    if (g_sock) {
        GList *each = NULL;
        g_printf ("shutting down\n");

        /* clean up clients */
        each = g_list_first (g_client_channels);
        while (each) {
            g_io_channel_shutdown (each->data, FALSE, 0);
            each = g_list_next (each);
        }

        each = g_list_first (g_client_sockets);
        while (each) {
            close (GPOINTER_TO_INT(each->data));
            each = g_list_next (each);
        }
        
        close (g_sock);
        g_sock = 0;
        gtk_main_quit ();
    }
}

int main (int argc, char *argv[]) {
    GtkWidget *setup;

    gtk_init (&argc, &argv);
    signal (SIGINT, catch_int);

    setup = create_setup ();
    gtk_widget_show (setup);

    gtk_main();
    
    return 0;
}
