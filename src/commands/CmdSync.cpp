////////////////////////////////////////////////////////////////////////////////
// taskwarrior - a command line task list manager.
//
// Copyright 2006-2012, Paul Beckingham, Federico Hernandez.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// http://www.opensource.org/licenses/mit-license.php
//
////////////////////////////////////////////////////////////////////////////////

#include <cmake.h>
#include <sstream>
#include <inttypes.h>
#include <Context.h>
#include <TLSClient.h>
#include <Color.h>
#include <text.h>
#include <util.h>
#include <i18n.h>
#include <CmdSync.h>

extern Context context;

////////////////////////////////////////////////////////////////////////////////
CmdSync::CmdSync ()
{
  _keyword     = "synchronize";
  _usage       = "task          synchronize [initialize]";
  _description = STRING_CMD_SYNC_USAGE;
  _read_only   = false;
  _displays_id = false;
}

////////////////////////////////////////////////////////////////////////////////
int CmdSync::execute (std::string& output)
{
  int status = 0;
#ifdef HAVE_LIBGNUTLS
  std::stringstream out;

  // Loog for the 'init' keyword to indicate one-time pending.data upload.
  bool first_time_init = false;
  std::vector <std::string> words = context.a3.extract_words ();
  std::vector <std::string>::iterator word;
  for (word = words.begin (); word != words.end (); ++word)
  {
    if (closeEnough ("initialize", *word, 4))
    {
      if (!context.config.getBoolean ("confirmation") ||
          confirm (STRING_CMD_SYNC_INIT))
        first_time_init = true;
      else
        throw std::string (STRING_CMD_SYNC_NO_INIT);
    }
  }

  // If no server is set up, quit.
  std::string connection = context.config.get ("taskd.server");
  if (connection == "" ||
      connection.rfind (':') == std::string::npos)
    throw std::string (STRING_CMD_SYNC_NO_SERVER);

  // Obtain credentials.
  std::string credentials_string = context.config.get ("taskd.credentials");
  if (credentials_string == "")
    throw std::string (STRING_CMD_SYNC_BAD_CRED);

  std::vector <std::string> credentials;
  split (credentials, credentials_string, "/");
  if (credentials.size () != 3)
    throw std::string (STRING_CMD_SYNC_BAD_CRED);

  std::string certificate = context.config.get ("taskd.certificate");
  if (certificate == "")
    throw std::string (STRING_CMD_SYNC_BAD_CERT);

  // If this is a first-time initialization, send pending.data, not
  // backlog.data.
  std::string payload = "";
  int upload_count = 0;
  if (first_time_init)
  {
    std::vector <Task> pending = context.tdb2.pending.get_tasks ();
    std::vector <Task>::iterator i;
    for (i = pending.begin (); i != pending.end (); ++i)
    {
      payload += i->composeJSON () + "\n";
      ++upload_count;
    }
  }
  else
  {
    std::vector <std::string> lines = context.tdb2.backlog.get_lines ();
    std::vector <std::string>::iterator i;
    for (i = lines.begin (); i != lines.end (); ++i)
    {
      if ((*i)[0] == '{')
        ++upload_count;

      payload += *i + "\n";
    }
  }

  // Send 'sync' + payload.
  Msg request;
  request.set ("protocol", "v1");
  request.set ("type",     "sync");
  request.set ("org",      credentials[0]);
  request.set ("user",     credentials[1]);
  request.set ("key",      credentials[2]);

  request.setPayload (payload);

  out << format (STRING_CMD_SYNC_PROGRESS, connection)
      << "\n";

  Msg response;
  if (send (connection, certificate, request, response))
  {
    std::string code = response.get ("code");
    if (code == "200")
    {
      Color colorAdded    (context.config.get ("color.sync.added"));
      Color colorChanged  (context.config.get ("color.sync.changed"));

      int download_count = 0;
      payload = response.getPayload ();
      std::vector <std::string> lines;
      split (lines, payload, '\n');

      // Load all tasks, but only if necessary.  There is always a sync key in
      // the payload, so if there are two or more lines, then we have merging
      // to perform, otherwise it's just a backlog.data update.
      if (lines.size () > 1)
        context.tdb2.all_tasks ();

      std::string synch_key = "";
      std::vector <std::string>::iterator line;
      for (line = lines.begin (); line != lines.end (); ++line)
      {
        if ((*line)[0] == '{')
        {
          ++download_count;

          Task from_server (*line);
          std::string uuid = from_server.get ("uuid");

          // Is it a new task from the server, or an update to an existing one?
          Task dummy;
          if (context.tdb2.get (uuid, dummy))
          {
            out << "  "
                << colorChanged.colorize (
                     format (STRING_CMD_SYNC_MOD,
                             uuid,
                             from_server.get ("description")))
                << "\n";
            context.tdb2.modify (from_server, false);
          }
          else
          {
            out << "  "
                << colorAdded.colorize (
                     format (STRING_CMD_SYNC_ADD,
                             uuid,
                             from_server.get ("description")))
                << "\n";
            context.tdb2.add (from_server, false);
          }
        }
        else if (*line != "")
        {
          synch_key = *line;
          context.debug ("Synch key " + synch_key);
        }

        // Otherwise line is blank, so ignore it.
      }

      // Only update everything if there is a new synch_key.  No synch_key means
      // something horrible happened on the other end of the wire.
      if (synch_key != "")
      {
        // Truncate backlog.data, save new synch_key.
        context.tdb2.backlog._file.truncate ();
        context.tdb2.backlog.clear_tasks ();
        context.tdb2.backlog.clear_lines ();
        context.tdb2.backlog.add_line (synch_key + "\n");

        // Commit all changes.
        context.tdb2.commit ();

        // Present a clear status message.
        if (upload_count == 0 && download_count == 0)
          // Note: should not happen - expect code 201 instead.
          context.footnote (STRING_CMD_SYNC_SUCCESS0);
        else if (upload_count == 0 && download_count > 0)
          context.footnote (format (STRING_CMD_SYNC_SUCCESS2, download_count));
        else if (upload_count > 0 && download_count == 0)
          context.footnote (format (STRING_CMD_SYNC_SUCCESS1, upload_count));
        else if (upload_count > 0 && download_count > 0)
          context.footnote (format (STRING_CMD_SYNC_SUCCESS3, upload_count, download_count));
      }
    }
    else if (code == "201")
    {
      context.footnote (STRING_CMD_SYNC_SUCCESS_NOP);
    }
    else if (code == "301")
    {
      std::string new_server = response.get ("info");
      context.config.set ("taskd.server", new_server);
      context.error (STRING_CMD_SYNC_RELOCATE0);
      context.error ("  " + format (STRING_CMD_SYNC_RELOCATE1, new_server));
    }
    else if (code == "430")
    {
      context.error (STRING_CMD_SYNC_FAIL_ACCOUNT);
      status = 2;
    }
    else
    {
      context.error (format (STRING_CMD_SYNC_FAIL_ERROR,
                             code,
                             response.get ("status")));
      status = 2;
    }

    // Display all errors returned.  This is recommended by the server protocol.
    std::string to_be_displayed = response.get ("messages");
    if (to_be_displayed != "")
    {
      if (context.verbose ("footnote"))
        context.footnote (to_be_displayed);
      else
        context.debug (to_be_displayed);
    }
  }

  // Some kind of low-level error:
  //   - Server down
  //   - Wrong address
  //   - Wrong port
  //   - Firewall
  //   - Network error
  //   - No signal/cable
  else
  {
    context.error (STRING_CMD_SYNC_FAIL_CONNECT);
    status = 1;
  }

  out << "\n";
  output = out.str ();

#else
  // Without GnuTLS found at compile time, there is no working sync command.
  throw std::string (STRING_CMD_SYNC_NO_TLS);
#endif
  return status;
}

////////////////////////////////////////////////////////////////////////////////
bool CmdSync::send (
  const std::string& to,
  const std::string& certificate,
  const Msg& request,
  Msg& response)
{
#ifdef HAVE_LIBGNUTLS
  std::string::size_type colon = to.rfind (':');
  if (colon == std::string::npos)
    throw format (STRING_CMD_SYNC_BAD_SERVER, to);

  std::string server = to.substr (0, colon);
  std::string port = to.substr (colon + 1);

  File cert (certificate);

  try
  {
    // A very basic TLS client, with X.509 authentication.
    TLSClient client;
    client.debug (context.config.getInteger ("debug.tls"));
    client.init (cert);
    client.connect (server, port);

    client.send (request.serialize () + "\n");

    std::string incoming;
    client.recv (incoming);
    client.bye ();

    response.parse (incoming);
    return true;
  }

  catch (std::string& error)
  {
    context.debug (error);
  }

  // Indicate message failed.
#endif
  return false;
}

////////////////////////////////////////////////////////////////////////////////
