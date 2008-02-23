/*MT*
    
    MediaTomb - http://www.mediatomb.cc/
    
    url_request_handler.cc - this file is part of MediaTomb.
    
    Copyright (C) 2005 Gena Batyan <bgeradz@mediatomb.cc>,
                       Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>
    
    Copyright (C) 2006-2008 Gena Batyan <bgeradz@mediatomb.cc>,
                            Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>,
                            Leonhard Wimmer <leo@mediatomb.cc>
    
    MediaTomb is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation.
    
    MediaTomb is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    version 2 along with MediaTomb; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
    
    $Id$
*/

/// \file url_request_handler.cc

#ifdef HAVE_CONFIG_H
    #include "autoconfig.h"
#endif

#ifdef HAVE_CURL

#include "server.h"
#include "common.h"
#include "storage.h"
#include "ixml.h"
#include "buffered_io_handler.h"
#include "dictionary.h"
#include "url_request_handler.h"
#include "cds_objects.h"
#ifdef ONLINE_SERVICES
    #include "online_service_helper.h"
#endif
#include "url.h"
#include "curl_io_handler.h"
#ifdef EXTERNAL_TRANSCODING
    #include "transcoding/transcode_dispatcher.h"
#endif

using namespace zmm;
using namespace mxml;

URLRequestHandler::URLRequestHandler() : RequestHandler()
{
}

void URLRequestHandler::get_info(IN const char *filename, OUT struct File_Info *info)
{
    log_debug("start\n");

    String mimeType;
    int objectID;
#ifdef EXTERNAL_TRANSCODING
    String tr_profile;
#endif

    String url, parameters;
    split_url(filename, URL_PARAM_SEPARATOR, url, parameters);
    
    Ref<Dictionary> dict(new Dictionary());
    dict->decode(parameters);

    log_debug("full url (filename): %s, url: %s, parameters: %s\n",
               filename, url.c_str(), parameters.c_str());
    
    String objID = dict->get(_("object_id"));
    if (objID == nil)
    {
        //log_error("object_id not found in url\n");
        throw _Exception(_("get_info: object_id not found"));
    }
    else
        objectID = objID.toInt();

    //log_debug("got ObjectID: [%s]\n", object_id.c_str());

    Ref<Storage> storage = Storage::getInstance();

    Ref<CdsObject> obj = storage->loadObject(objectID);

    int objectType = obj->getObjectType();

    if (!IS_CDS_ITEM_EXTERNAL_URL(objectType))
    {
        throw _Exception(_("get_info: object is not an external url item"));
    }

#ifdef EXTERNAL_TRANSCODING
    tr_profile = dict->get(_(URL_PARAM_TRANSCODE_PROFILE_NAME));

    if (string_ok(tr_profile))
    {
        Ref<TranscodingProfile> tp = ConfigManager::getInstance()->getTranscodingProfileListOption(CFG_TRANSCODING_PROFILE_LIST)->getByName(tr_profile);

        if (tp == nil)
            throw _Exception(_("Transcoding requested but no profile "
                               "matching the name ") + tr_profile + " found");

        mimeType = tp->getTargetMimeType();
        info->file_length = -1;
    }
    else
#endif
    {
        Ref<CdsItemExternalURL> item = RefCast(obj, CdsItemExternalURL);

#ifdef ONLINE_SERVICES
        if (item->getFlag(OBJECT_FLAG_ONLINE_SERVICE))
        {
            /// \todo write a helper class that will handle various online
            /// services
            Ref<OnlineServiceHelper> helper (new OnlineServiceHelper());
            url = helper->resolveURL(item);
        }
        else
#endif
        {
            url = item->getLocation();
        }

        log_debug("Online content url: %s\n", url.c_str());
        Ref<URL> u(new URL(1024));
        Ref<URL::Stat> st;
        try
        { 
            st = u->getInfo(url);
            info->file_length = st->getSize();
        }
        catch (Exception ex)
        {
            log_warning("%s\n", ex.getMessage().c_str());
            info->file_length = -1;
        }

        mimeType = item->getMimeType();
    }

    info->is_readable = 1;
    info->last_modified = 0;
    info->is_directory = 0;
    info->http_header = NULL;

    info->content_type = ixmlCloneDOMString(mimeType.c_str());
    log_debug("web_get_info(): end\n");

    /// \todo transcoding for get_info
}

Ref<IOHandler> URLRequestHandler::open(IN const char *filename, OUT struct File_Info *info, IN enum UpnpOpenFileMode mode)
{
    int objectID;
    String mimeType;
#ifdef EXTERNAL_TRANSCODING
    String tr_profile;
#endif

    log_debug("start\n");

    // Currently we explicitly do not support UPNP_WRITE
    // due to security reasons.
    if (mode != UPNP_READ)
        throw _Exception(_("UPNP_WRITE unsupported"));

    String url, parameters;
    split_url(filename, URL_PARAM_SEPARATOR, url, parameters);

    Ref<Dictionary> dict(new Dictionary());
    dict->decode(parameters);
    log_debug("full url (filename): %s, url: %s, parameters: %s\n",
               filename, url.c_str(), parameters.c_str());

    String objID = dict->get(_("object_id"));
    if (objID == nil)
    {
        throw _Exception(_("object_id not found"));
    }
    else
        objectID = objID.toInt();

    Ref<Storage> storage = Storage::getInstance();

    Ref<CdsObject> obj = storage->loadObject(objectID);

    int objectType = obj->getObjectType();

    if (!IS_CDS_ITEM_EXTERNAL_URL(objectType))
    {
        throw _Exception(_("object is not an external url item"));
    }

    Ref<CdsItemExternalURL> item = RefCast(obj, CdsItemExternalURL);

#ifdef ONLINE_SERVICES
    if (item->getFlag(OBJECT_FLAG_ONLINE_SERVICE))
    {
        Ref<OnlineServiceHelper> helper (new OnlineServiceHelper());
        url = helper->resolveURL(item);
    }
    else 
#endif
    {
        url = item->getLocation();
    }

    log_debug("Online content url: %s\n", url.c_str());

    info->is_readable = 1;
    info->last_modified = 0;
    info->is_directory = 0;
    info->http_header = NULL;

#ifdef EXTERNAL_TRANSCODING
    tr_profile = dict->get(_(URL_PARAM_TRANSCODE_PROFILE_NAME));

    if (string_ok(tr_profile))
    {
        Ref<TranscodingProfile> tp = ConfigManager::getInstance()->getTranscodingProfileListOption(CFG_TRANSCODING_PROFILE_LIST)->getByName(tr_profile);

        if (tp == nil)
            throw _Exception(_("Transcoding of file ") + url +
                    " but no profile matching the name " +
                    tr_profile + " found");

        Ref<TranscodeDispatcher> tr_d(new TranscodeDispatcher());
        return tr_d->open(tp, url, item->getObjectType(), info);
    }
    else
#endif
    {
        Ref<URL> u(new URL(1024));
        Ref<URL::Stat> st;
        try
        {
            st = u->getInfo(url);
            info->file_length = st->getSize();
        }
        catch (Exception ex)
        {
            log_warning("%s\n", ex.getMessage().c_str());
            info->file_length = -1;
        }
        mimeType = item->getMimeType();
        info->content_type = ixmlCloneDOMString(mimeType.c_str());
    }

    printf("TODO: make curl io handler configurable for url request handler\n");
    Ref<IOHandler> io_handler(new CurlIOHandler(url, NULL, 1024*1024, 0));

    io_handler->open(mode);
    return io_handler;
}

#endif // HAVE_CURL

