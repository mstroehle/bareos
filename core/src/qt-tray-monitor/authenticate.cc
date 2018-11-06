/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2004-2008 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2013 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Bareos authentication. Provides authentication with File and Storage daemons.
 *
 * Nicolas Boichat, August MMIV
 */

#include "monitoritem.h"
#include "authenticate.h"
#include "include/jcr.h"
#include "monitoritemthread.h"

#include "lib/tls_conf.h"
#include "lib/bnet.h"
#include "lib/qualified_resource_name_type_converter.h"

const int debuglevel = 50;

/* Commands sent to Storage daemon and File daemon and received
 *  from the User Agent */
static char SDFDhello[]    = "Hello Director %s calling\n";

/* Response from SD */
static char SDOKhello[]   = "3000 OK Hello\n";
/* Response from FD */
static char FDOKhello[] = "2000 OK Hello";

static std::map<AuthenticationResult,std::string> authentication_error_to_string_map {
  { AuthenticationResult::kNoError, "No Error" },
  { AuthenticationResult::kAlreadyAuthenticated, "Already authenticated" },
  { AuthenticationResult::kQualifiedResourceNameFailed, "Could not generate a qualified resource name" },
  { AuthenticationResult::kTlsHandshakeFailed, "TLS Handshake failed" },
  { AuthenticationResult::kSendHelloMessageFailed, "Send of hello handshake message failed" },
  { AuthenticationResult::kCramMd5HandshakeFailed, "Challenge response handshake failed" },
  { AuthenticationResult::kDaemonResponseFailed, "Daemon response could not be read" },
  { AuthenticationResult::kRejectedByDaemon, "Authentication was rejected by the daemon" },
  { AuthenticationResult::kUnknownDaemon, "Unkown daemon type" }
};

bool GetAuthenticationResultString(AuthenticationResult err, std::string &buffer)
{
  if (authentication_error_to_string_map.find(err) != authentication_error_to_string_map.end() ) {
    buffer = authentication_error_to_string_map.at(err);
    return true;
  }
  return false;
}

static AuthenticationResult AuthenticateWithDirector(JobControlRecord *jcr, DirectorResource *dir_res)
{
   if (jcr->authenticated) {
      return AuthenticationResult::kAlreadyAuthenticated;
   }

   BareosSocket *dir = jcr->dir_bsock;
   MonitorResource *monitor = MonitorItemThread::instance()->getMonitor();
   if (IsTlsConfigured(dir_res)) {
     std::string qualified_resource_name;
     if (!my_config->GetQualifiedResourceNameTypeConverter()->ResourceToString(
                    monitor->name(), R_CONSOLE, qualified_resource_name)) {
       return AuthenticationResult::kQualifiedResourceNameFailed;
     }

     if (!dir->DoTlsHandshake(TlsConfigBase::BNET_TLS_AUTO, dir_res, false, qualified_resource_name.c_str(), monitor->password.value, jcr)) {
        return AuthenticationResult::kTlsHandshakeFailed;
     }
   }

   char errmsg[1024];
   int32_t errmsg_len = sizeof(errmsg);
   if (!dir->AuthenticateWithDirector(jcr, monitor->name(),(s_password &) monitor->password, errmsg, errmsg_len, dir_res)) {
      Jmsg(jcr, M_FATAL, 0, _("Director authorization problem.\n"
                              "Most likely the passwords do not agree.\n"
                              "Please see %s for help.\n"), MANUAL_AUTH_URL);
      return AuthenticationResult::kCramMd5HandshakeFailed;
   }

   return AuthenticationResult::kNoError;
}

static AuthenticationResult AuthenticateWithStorageDaemon(JobControlRecord *jcr, StorageResource* store)
{
   if (jcr->authenticated) {
      return AuthenticationResult::kAlreadyAuthenticated;
   }

   BareosSocket *sd = jcr->store_bsock;
   MonitorResource *monitor = MonitorItemThread::instance()->getMonitor();
   if (IsTlsConfigured(store)) {
     std::string qualified_resource_name;
     if (!my_config->GetQualifiedResourceNameTypeConverter()->ResourceToString(
                    monitor->name(), R_DIRECTOR, qualified_resource_name)) {
       return AuthenticationResult::kQualifiedResourceNameFailed;
     }

     if (!sd->DoTlsHandshake(TlsConfigBase::BNET_TLS_AUTO, store, false,
                             qualified_resource_name.c_str(), store->password.value, jcr)) {
        return AuthenticationResult::kTlsHandshakeFailed;
     }
   }

   /**
    * Send my name to the Storage daemon then do authentication
    */
   char dirname[MAX_NAME_LENGTH];
   bstrncpy(dirname, monitor->name(), sizeof(dirname));
   BashSpaces(dirname);

   if (!sd->fsend(SDFDhello, dirname)) {
      Dmsg1(debuglevel, _("Error sending Hello to Storage daemon. ERR=%s\n"), BnetStrerror(sd));
      Jmsg(jcr, M_FATAL, 0, _("Error sending Hello to Storage daemon. ERR=%s\n"), BnetStrerror(sd));
      return AuthenticationResult::kSendHelloMessageFailed;
   }

   bool auth_success = sd->AuthenticateOutboundConnection(
      jcr, "Storage daemon", dirname, store->password, store);
   if (!auth_success) {
      Dmsg2(debuglevel,
            "Director unable to authenticate with Storage daemon at \"%s:%d\"\n",
            sd->host(),
            sd->port());
      Jmsg(jcr, M_FATAL, 0,
           _("Director unable to authenticate with Storage daemon at \"%s:%d\". Possible causes:\n"
                "Passwords or names not the same or\n"
                "TLS negotiation problem or\n"
                "Maximum Concurrent Jobs exceeded on the SD or\n"
                "SD networking messed up (restart daemon).\n"
                "Please see %s for help.\n"),
           sd->host(), sd->port(), MANUAL_AUTH_URL);
      return AuthenticationResult::kCramMd5HandshakeFailed;
   }

   Dmsg1(116, ">stored: %s", sd->msg);
   if (sd->recv() <= 0) {
      Jmsg3(jcr, M_FATAL, 0, _("dir<stored: \"%s:%s\" bad response to Hello command: ERR=%s\n"),
            sd->who(), sd->host(), sd->bstrerror());
      return AuthenticationResult::kDaemonResponseFailed;
   }

   Dmsg1(110, "<stored: %s", sd->msg);
   if (!bstrncmp(sd->msg, SDOKhello, sizeof(SDOKhello))) {
      Dmsg0(debuglevel, _("Storage daemon rejected Hello command\n"));
      Jmsg2(jcr, M_FATAL, 0, _("Storage daemon at \"%s:%d\" rejected Hello command\n"),
            sd->host(), sd->port());
      return AuthenticationResult::kRejectedByDaemon;
   }

   return AuthenticationResult::kNoError;
}

static AuthenticationResult AuthenticateWithFileDaemon(JobControlRecord *jcr, ClientResource* client)
{
   if (jcr->authenticated) {
      return AuthenticationResult::kAlreadyAuthenticated;
   }

   BareosSocket *fd = jcr->file_bsock;
   MonitorResource *monitor = MonitorItemThread::instance()->getMonitor();
   if (IsTlsConfigured(client)) {
     std::string qualified_resource_name;
     if (!my_config->GetQualifiedResourceNameTypeConverter()->ResourceToString(
                    monitor->name(), R_DIRECTOR, qualified_resource_name)) {
       return AuthenticationResult::kQualifiedResourceNameFailed;
     }

     if (!fd->DoTlsHandshake(TlsConfigBase::BNET_TLS_AUTO, client, false,
                             qualified_resource_name.c_str(), client->password.value, jcr)) {
        return AuthenticationResult::kTlsHandshakeFailed;
     }
   }

   /**
    * Send my name to the File daemon then do authentication
    */
   char dirname[MAX_NAME_LENGTH];
   bstrncpy(dirname, monitor->name(), sizeof(dirname));
   BashSpaces(dirname);

   if (!fd->fsend(SDFDhello, dirname)) {
      Jmsg(jcr, M_FATAL, 0, _("Error sending Hello to File daemon at \"%s:%d\". ERR=%s\n"),
           fd->host(), fd->port(), fd->bstrerror());
      return AuthenticationResult::kSendHelloMessageFailed;
   }
   Dmsg1(debuglevel, "Sent: %s", fd->msg);

   bool auth_success =
      fd->AuthenticateOutboundConnection(jcr, "File Daemon", dirname, client->password, client);

   if (!auth_success) {
      Dmsg2(debuglevel, "Unable to authenticate with File daemon at \"%s:%d\"\n", fd->host(), fd->port());
      Jmsg(jcr,
           M_FATAL,
           0,
           _("Unable to authenticate with File daemon at \"%s:%d\". Possible causes:\n"
                "Passwords or names not the same or\n"
                "TLS negotiation failed or\n"
                "Maximum Concurrent Jobs exceeded on the FD or\n"
                "FD networking messed up (restart daemon).\n"
                "Please see %s for help.\n"),
           fd->host(),
           fd->port(),
           MANUAL_AUTH_URL);
      return AuthenticationResult::kCramMd5HandshakeFailed;
   }

   Dmsg1(116, ">filed: %s", fd->msg);
   if (fd->recv() <= 0) {
      Dmsg1(debuglevel, _("Bad response from File daemon to Hello command: ERR=%s\n"),
            BnetStrerror(fd));
      Jmsg(jcr, M_FATAL, 0, _("Bad response from File daemon at \"%s:%d\" to Hello command: ERR=%s\n"),
           fd->host(), fd->port(), fd->bstrerror());
      return AuthenticationResult::kDaemonResponseFailed;
   }

   Dmsg1(110, "<filed: %s", fd->msg);
   if (strncmp(fd->msg, FDOKhello, sizeof(FDOKhello)-1) != 0) {
      Jmsg(jcr, M_FATAL, 0, _("File daemon rejected Hello command\n"));
      return AuthenticationResult::kRejectedByDaemon;
   }

   return AuthenticationResult::kNoError;
}

AuthenticationResult AuthenticateWithDaemon(MonitorItem* item, JobControlRecord *jcr)
{
   switch (item->type()) {
   case R_DIRECTOR:
      return AuthenticateWithDirector(jcr, (DirectorResource*)item->resource());
   case R_CLIENT:
      return AuthenticateWithFileDaemon(jcr, (ClientResource*)item->resource());
   case R_STORAGE:
      return AuthenticateWithStorageDaemon(jcr, (StorageResource*)item->resource());
   default:
      printf(_("Error, currentitem is not a Client or a Storage..\n"));
      return AuthenticationResult::kUnknownDaemon;
   }
}
