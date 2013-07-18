/*
 * Copyright (c) 2011, Psiphon Inc.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include "transport_connection.h"
#include "systemproxysettings.h"
#include "server_request.h"
#include "local_proxy.h"
#include "transport.h"
#include "psiclient.h"
#include "config.h"
#include "embeddedvalues.h"


TransportConnection::TransportConnection()
    : m_transport(0),
      m_localProxy(0)
{
}

TransportConnection::~TransportConnection()
{
    Cleanup();
}

SessionInfo TransportConnection::GetUpdatedSessionInfo() const
{
    return m_sessionInfo;
}

void TransportConnection::Connect(
                            const StopInfo& stopInfo,
                            ITransport* transport,
                            ILocalProxyStatsCollector* statsCollector, 
                            ServerEntries& io_serverEntries, // may be modified
                            const tstring& splitTunnelingFilePath,
                            bool disallowHandshake)
{
    assert(m_transport == 0);
    assert(m_localProxy == 0); 

    assert(transport);
    assert(io_serverEntries.size() > 0);

    // To prevent unnecessary complexity, we're going to assume certain things
    // about the transport type and multi-connect (i.e., parallel connection
    // attempts) capabilities. Specifically, if the transport wants to 
    // multi-connect, then it should not require pre-handshakes. Handshakes 
    // are done serially, so it would undermine the point of multi-connect if 
    // they preceded the connection.
    // If pre-handshake is required, we're going to enforce that only one
    // connection attempt will be made (at a time).
    if (transport->IsHandshakeRequired(io_serverEntries.front()))
    {
        assert(io_serverEntries.size() == 1);
        assert(transport->GetMultiConnectCount() == 1);

        // Even though we've done those debug checks, we're going to enforce
        // the rules.
        io_serverEntries.resize(1);

        // If the caller demands that we not do a handshake, then we can go 
        // no further.
        if (disallowHandshake)
        {
            throw TryNextServer();
        }
    }
    else // no pre-handshake for the first server entry
    {
        // Remove all server entries that do require a pre-handshake.
        for (int i = io_serverEntries.size()-1; i >= 0; i--)
        {
            if (transport->IsHandshakeRequired(io_serverEntries[i]))
            {
                io_serverEntries.erase(io_serverEntries.begin()+i);
            }
        }

        // Trim the server entries vector to be at most as many as the 
        // transport can handle at once.
        if (io_serverEntries.size() > transport->GetMultiConnectCount())
        {
            io_serverEntries.resize(transport->GetMultiConnectCount());
        }
    }
    // Now the server entries vector only contains items that are valid to the
    // multi-connect type of the transport, and either all need pre-handshake
    // or all do not.

    // Create a vector of SessionInfo structs that use the ServerEntries.
    vector<SessionInfo> sessionInfoCandidates;
    sessionInfoCandidates.resize(io_serverEntries.size());
    for (size_t i = 0; i < io_serverEntries.size(); i++)
    {
        sessionInfoCandidates[i].Set(io_serverEntries[i]);
    }

    m_transport = transport;
    bool handshakeDone = false;

    try
    {
        // Delete any leftover split tunnelling rules
        if(splitTunnelingFilePath.length() > 0 && !DeleteFile(splitTunnelingFilePath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            throw std::exception("TransportConnection::Connect - DeleteFile failed");
        }

        // Some transports require a handshake before connecting; with others we
        // can connect before doing the handshake.    
        if (m_transport->IsHandshakeRequired(io_serverEntries.front()))
        {
            my_print(NOT_SENSITIVE, true, _T("%s: Doing pre-handshake; insufficient server info for immediate connection"), __TFUNCTION__);

            if (!DoHandshake(
                    true, // pre-handshake
                    stopInfo, 
                    sessionInfoCandidates.front(), 
                    io_serverEntries))
            {
                // Need a handshake but can't do a handshake or handshake failing.
                throw TryNextServer();
            }

            handshakeDone = true;
        }
        else
        {
            my_print(NOT_SENSITIVE, true, _T("%s: Not doing pre-handshake; enough server info for immediate connection"), __TFUNCTION__);
        }

        m_workerThreadSynch.Reset();

        int chosenSessionInfoIndex = -1;
        // Connect with the transport. Will throw on error.
        // Note that this may attempt parallel connections internally.
        m_transport->Connect(
                    sessionInfoCandidates, 
                    &m_systemProxySettings,
                    stopInfo,
                    &m_workerThreadSynch,
                    chosenSessionInfoIndex);

        assert(chosenSessionInfoIndex >= 0 && chosenSessionInfoIndex < (signed)sessionInfoCandidates.size());
        m_sessionInfo = sessionInfoCandidates[chosenSessionInfoIndex];

        // Set up and start the local proxy.
        m_localProxy = new LocalProxy(
                            statsCollector, 
                            m_sessionInfo, 
                            &m_systemProxySettings,
                            m_transport->GetLocalProxyParentPort(), 
                            splitTunnelingFilePath);

        // Launches the local proxy thread and doesn't return until it
        // observes a successful (or not) connection.
        if (!m_localProxy->Start(stopInfo, &m_workerThreadSynch))
        {
            throw IWorkerThread::Error("LocalProxy::Start failed");
        }

        // Apply the system proxy settings that have been collected by the transport
        // and the local proxy.
        m_systemProxySettings.Apply();

        // If we didn't do the handshake before, do it now.
        if (!handshakeDone && !disallowHandshake)
        {
            if (!DoHandshake(
                    false, // not pre-handshake
                    stopInfo, 
                    m_sessionInfo,
                    io_serverEntries))
            {
                my_print(NOT_SENSITIVE, true, _T("%s: Post-handshake failed"), __TFUNCTION__);
            }
            // We do not fail regardless of whether the handshake succeeds.

            handshakeDone = true;
        }

        // Now that we have extra info from the server via the handshake 
        // (specifically page view regexes), we need to update the local proxy.
        m_localProxy->UpdateSessionInfo(m_sessionInfo);

        // We also need to update the transport session, in case anything has 
        // changed or been filled in.
        m_transport->UpdateSessionInfo(m_sessionInfo);
    }
    catch (ITransport::TransportFailed&)
    {
        Cleanup();

        // We don't fail over transports, so...
        throw TransportConnection::TryNextServer();
    }
    catch(...)
    {
        // Make sure the transport and proxy are cleaned up and then just rethrow
        Cleanup();

        throw;
    }
}

void TransportConnection::WaitForDisconnect()
{
    HANDLE waitHandles[] = { m_transport->GetStoppedEvent(), 
                             m_localProxy->GetStoppedEvent() };
    size_t waitHandlesCount = sizeof(waitHandles)/sizeof(HANDLE);

    DWORD result = WaitForMultipleObjects(
                    waitHandlesCount, 
                    waitHandles, 
                    FALSE, // wait for any event
                    INFINITE);

    // One of the transport or the local proxy has stopped. 
    // Make sure they both are.
    m_localProxy->Stop();
    m_transport->Stop();

    Cleanup();

    if (result > (WAIT_OBJECT_0 + waitHandlesCount - 1))
    {
        throw IWorkerThread::Error("WaitForMultipleObjects failed");
    }
}

bool TransportConnection::DoHandshake(
                            bool preTransport,
                            const StopInfo& stopInfo, 
                            SessionInfo& sessionInfo, 
                            const ServerEntries& serverEntries)
{
    string handshakeResponse;

    tstring handshakeRequestPath = GetHandshakeRequestPath(sessionInfo, serverEntries);

    // Send list of known server IP addresses (used for stats logging on the server)

    // Allow an adhoc tunnel if this is a pre-transport handshake (i.e, for VPN)
    ServerRequest::ReqLevel reqLevel = preTransport ? ServerRequest::FULL : ServerRequest::ONLY_IF_TRANSPORT;

    if (!ServerRequest::MakeRequest(
                        reqLevel,
                        m_transport,
                        sessionInfo,
                        handshakeRequestPath.c_str(),
                        handshakeResponse,
                        stopInfo)
        || handshakeResponse.length() <= 0)
    {
        my_print(NOT_SENSITIVE, false, _T("Handshake failed"));
        return false;
    }

    if (!sessionInfo.ParseHandshakeResponse(handshakeResponse.c_str()))
    {
        // If the handshake parsing has failed, something is very wrong.
        my_print(NOT_SENSITIVE, false, _T("%s: ParseHandshakeResponse failed"), __TFUNCTION__);
        throw TryNextServer();
    }

    return true;
}

tstring TransportConnection::GetHandshakeRequestPath(
                                const SessionInfo& sessionInfo, 
                                const ServerEntries& serverEntries)
{
    tstring handshakeRequestPath;
    handshakeRequestPath = tstring(HTTP_HANDSHAKE_REQUEST_PATH) + 
                           _T("?client_session_id=") + NarrowToTString(sessionInfo.GetClientSessionID()) +
                           _T("&propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
                           _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
                           _T("&client_version=") + NarrowToTString(CLIENT_VERSION) +
                           _T("&server_secret=") + NarrowToTString(sessionInfo.GetWebServerSecret()) +
                           _T("&relay_protocol=") + m_transport->GetTransportProtocolName();

    // Include a list of known server IP addresses in the request query string as required by /handshake
    for (ServerEntryIterator ii = serverEntries.begin(); ii != serverEntries.end(); ++ii)
    {
        handshakeRequestPath += _T("&known_server=");
        handshakeRequestPath += NarrowToTString(ii->serverAddress);
    }

    return handshakeRequestPath;
}

void TransportConnection::Cleanup()
{
    // NOTE: It is important that the system proxy settings get torn down
    // before the transport and local proxy do. Otherwise, all web connections
    // will have a window of being guaranteed to fail (including and especially
    // our own -- like final /status requests).
    m_systemProxySettings.Revert();

    if (m_transport)
    {
        m_transport->Stop();
        m_transport->Cleanup();
    }

    if (m_localProxy) delete m_localProxy;
    m_localProxy = 0;
}
