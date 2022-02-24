// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2021 Andy Holmes <andrew.g.r.holmes@gmail.com>

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <libvalent-core.h>
#include <libvalent-test.h>


typedef struct
{
  GMainLoop            *loop;
  JsonNode             *packets;

  ValentChannelService *service;
  ValentChannel        *channel;
  ValentChannel        *endpoint;

  gpointer              data;
} ChannelServiceFixture;

static void
channel_service_fixture_set_up (ChannelServiceFixture *fixture,
                                gconstpointer      user_data)
{
  PeasPluginInfo *plugin_info;

  fixture->loop = g_main_loop_new (NULL, FALSE);

  plugin_info = peas_engine_get_plugin_info (valent_get_engine (), "mock");
  fixture->service = g_object_new (VALENT_TYPE_MOCK_CHANNEL_SERVICE,
                                   "id",          "mock-service",
                                   "name",        "Mock Service",
                                   "plugin-info", plugin_info,
                                   NULL);

  fixture->packets = valent_test_load_json (TEST_DATA_DIR"/core.json");
}

static void
channel_service_fixture_tear_down (ChannelServiceFixture *fixture,
                                   gconstpointer      user_data)
{
  g_clear_pointer (&fixture->loop, g_main_loop_unref);
  g_clear_pointer (&fixture->packets, json_node_unref);

  v_await_finalize_object (fixture->service);

  if (fixture->channel != NULL)
    v_await_finalize_object (fixture->channel);

  if (fixture->endpoint != NULL)
    v_await_finalize_object (fixture->endpoint);
}

/*
 * ValentChannelService Callbacks
 */
static void
start_cb (ValentChannelService  *service,
          GAsyncResult          *result,
          ChannelServiceFixture *fixture)
{
  gboolean ret;
  GError *error = NULL;

  ret = valent_channel_service_start_finish (service, result, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  g_main_loop_quit (fixture->loop);
}

static void
on_channel (ValentChannelService  *service,
            ValentChannel         *channel,
            ChannelServiceFixture *fixture)
{
  fixture->channel = channel;
  g_object_add_weak_pointer (G_OBJECT (fixture->channel),
                             (gpointer)&fixture->channel);
  g_main_loop_quit (fixture->loop);
}


/*
 * ValentChannel Callbacks
 */
static void
close_cb (ValentChannel         *channel,
          GAsyncResult          *result,
          ChannelServiceFixture *fixture)
{
  gboolean ret;
  GError *error = NULL;

  ret = valent_channel_close_finish (channel, result, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  g_main_loop_quit (fixture->loop);
}

static void
read_packet_cb (ValentChannel         *channel,
                GAsyncResult          *result,
                ChannelServiceFixture *fixture)
{
  g_autoptr (JsonNode) packet = NULL;
  GError *error = NULL;

  packet = valent_channel_read_packet_finish (channel, result, &error);
  g_assert_no_error (error);
  g_assert_true (VALENT_IS_PACKET (packet));

  v_assert_packet_type (packet, "kdeconnect.mock.echo");
  v_assert_packet_cmpstr (packet, "foo", ==, "bar");

  g_main_loop_quit (fixture->loop);
}

static void
write_packet_cb (ValentChannel         *channel,
                 GAsyncResult          *result,
                 ChannelServiceFixture *fixture)
{
  gboolean ret;
  GError *error = NULL;

  ret = valent_channel_write_packet_finish (channel, result, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  g_main_loop_quit (fixture->loop);
}

static void
on_upload (ValentChannel *endpoint,
                      GAsyncResult  *result,
                      gpointer       user_data)
{
  g_autoptr (JsonNode) packet = NULL;
  g_autoptr (GIOStream) stream = NULL;
  g_autoptr (GOutputStream) target = NULL;
  goffset payload_size, transferred;
  GError *error = NULL;

  /* We expect the packet to be properly populated with payload information */
  packet = valent_channel_read_packet_finish (endpoint, result, &error);
  g_assert_no_error (error);
  g_assert_true (VALENT_IS_PACKET (packet));
  g_assert_true (valent_packet_has_payload (packet));

  payload_size = valent_packet_get_payload_size (packet);
  g_assert_cmpint (payload_size, >, 0);

  /* We expect to be able to create a transfer stream from the packet */
  stream = valent_channel_download (endpoint, packet, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (G_IS_IO_STREAM (stream));

  /* We expect to be able to transfer the full payload */
  target = g_memory_output_stream_new_resizable ();
  transferred = g_output_stream_splice (target,
                                        g_io_stream_get_input_stream (stream),
                                        (G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                         G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET),
                                        NULL,
                                        &error);
  g_assert_no_error (error);
  g_assert_cmpint (transferred, ==, payload_size);
}


static void
test_channel_service_basic (void)
{
  /* g_autoptr (GMainLoop) loop = NULL; */
  g_autoptr (ValentChannelService) service = NULL;
  g_autoptr (ValentData) data = NULL;
  PeasPluginInfo *plugin_info;
  g_autoptr (ValentData) data_out = NULL;
  g_autofree char *id_out = NULL;
  g_autoptr (JsonNode) identity_out = NULL;
  g_autofree char *name_out = NULL;
  PeasPluginInfo *plugin_info_out;

  /* ValentChannelService */
  plugin_info = peas_engine_get_plugin_info (valent_get_engine (), "mock");
  data = valent_data_new (NULL, NULL);
  service = g_object_new (VALENT_TYPE_MOCK_CHANNEL_SERVICE,
                          "data",        data,
                          "id",          "mock-service",
                          "name",        "Mock Service",
                          "plugin-info", plugin_info,
                          NULL);

  g_object_get (service,
                "data",        &data_out,
                "id",          &id_out,
                "identity",    &identity_out,
                "name",        &name_out,
                "plugin-info", &plugin_info_out,
                NULL);

  g_assert_true (data_out == data);
  g_assert_cmpstr (id_out, ==, "mock-service");
  g_assert_true (VALENT_IS_PACKET (identity_out));
  g_assert_cmpstr (name_out, ==, "Mock Service");
  g_assert_true (plugin_info_out == plugin_info);
  g_boxed_free (PEAS_TYPE_PLUGIN_INFO, plugin_info_out);

  g_clear_pointer (&id_out, g_free);
  id_out = valent_channel_service_dup_id (service);
  g_assert_cmpstr (id_out, ==, "mock-service");
  g_assert_cmpstr (valent_channel_service_get_name (service), ==, "Mock Service");
}

static void
test_channel_service_identify (ChannelServiceFixture *fixture,
                               gconstpointer          user_data)
{
  valent_channel_service_start (fixture->service,
                                NULL,
                                (GAsyncReadyCallback)start_cb,
                                fixture);
  g_main_loop_run (fixture->loop);

  g_signal_connect (fixture->service,
                    "channel",
                    G_CALLBACK (on_channel),
                    fixture);
  valent_channel_service_identify (fixture->service, NULL);

  fixture->endpoint = valent_mock_channel_service_get_endpoint ();
  g_assert_true (VALENT_IS_CHANNEL (fixture->endpoint));
  g_object_add_weak_pointer (G_OBJECT (fixture->endpoint),
                             (gpointer)&fixture->endpoint);

  g_signal_handlers_disconnect_by_data (fixture->service, fixture);
  valent_channel_service_stop (fixture->service);
}

static void
test_channel_service_channel (ChannelServiceFixture *fixture,
                              gconstpointer          user_data)
{
  GError *error = NULL;
  JsonNode *packet;
  g_autoptr (GIOStream) base_stream_out = NULL;
  g_autoptr (JsonNode) identity_out = NULL;
  g_autoptr (JsonNode) peer_identity_out = NULL;
  const char *channel_verification;
  const char *endpoint_verification;
  g_autoptr (GFile) file = NULL;

  valent_channel_service_start (fixture->service,
                                NULL,
                                (GAsyncReadyCallback)start_cb,
                                fixture);
  g_main_loop_run (fixture->loop);

  g_signal_connect (fixture->service,
                    "channel",
                    G_CALLBACK (on_channel),
                    fixture);
  valent_channel_service_identify (fixture->service, NULL);

  fixture->endpoint = valent_mock_channel_service_get_endpoint ();
  g_assert_true (VALENT_IS_CHANNEL (fixture->endpoint));
  g_object_add_weak_pointer (G_OBJECT (fixture->endpoint),
                             (gpointer)&fixture->endpoint);

  /* Properties */
  g_object_get (fixture->channel,
                "base-stream",   &base_stream_out,
                "identity",      &identity_out,
                "peer-identity", &peer_identity_out,
                NULL);

  g_assert_true (G_IS_IO_STREAM (base_stream_out));
  g_assert_true (VALENT_IS_PACKET (identity_out));
  g_assert_true (VALENT_IS_PACKET (peer_identity_out));

  g_assert_true (json_node_equal (valent_channel_get_identity (fixture->channel), identity_out));
  g_assert_true (json_node_equal (valent_channel_get_peer_identity (fixture->channel), peer_identity_out));

  channel_verification = valent_channel_get_verification_key (fixture->channel);
  endpoint_verification = valent_channel_get_verification_key (fixture->endpoint);
  g_assert_nonnull (channel_verification);
  g_assert_nonnull (endpoint_verification);
  g_assert_cmpstr (channel_verification, ==, endpoint_verification);

  /* Packet Exchange */
  packet = json_object_get_member (json_node_get_object (fixture->packets),
                                   "test-echo");
  valent_channel_write_packet (fixture->channel,
                               packet,
                               NULL,
                               (GAsyncReadyCallback)write_packet_cb,
                               fixture);
  g_main_loop_run (fixture->loop);

  valent_channel_read_packet (fixture->endpoint,
                              NULL,
                              (GAsyncReadyCallback)read_packet_cb,
                              fixture);
  g_main_loop_run (fixture->loop);

  /* Transfers */
  file = g_file_new_for_path (TEST_DATA_DIR"image.png");
  packet = json_object_get_member (json_node_get_object (fixture->packets),
                                   "test-transfer");

  valent_channel_read_packet (fixture->endpoint,
                              NULL,
                              (GAsyncReadyCallback)on_upload,
                              NULL);
  valent_test_upload (fixture->channel, packet, file, &error);
  g_assert_no_error (error);

  /* Closing */
  valent_channel_close_async (fixture->endpoint,
                              NULL,
                              (GAsyncReadyCallback)close_cb,
                              fixture);
  g_main_loop_run (fixture->loop);
  valent_channel_close (fixture->channel, NULL, NULL);

  g_signal_handlers_disconnect_by_data (fixture->service, fixture);
  valent_channel_service_stop (fixture->service);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

  g_type_ensure (VALENT_TYPE_MOCK_CHANNEL);
  g_type_ensure (VALENT_TYPE_MOCK_CHANNEL_SERVICE);

  g_test_add_func ("/core/channel-service/basic",
                   test_channel_service_basic);

  g_test_add ("/core/channel-service/identify",
              ChannelServiceFixture, NULL,
              channel_service_fixture_set_up,
              test_channel_service_identify,
              channel_service_fixture_tear_down);

  g_test_add ("/core/channel-service/channel",
              ChannelServiceFixture, NULL,
              channel_service_fixture_set_up,
              test_channel_service_channel,
              channel_service_fixture_tear_down);

  return g_test_run ();
}