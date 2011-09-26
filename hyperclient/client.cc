// Copyright (c) 2011, Cornell University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of HyperDex nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#define __STDC_LIMIT_MACROS

// STL
#include <queue>

// po6
#include <po6/net/socket.h>

// e
#include <e/bitfield.h>
#include <e/intrusive_ptr.h>
#include <e/timer.h>

// HyperspaceHashing
#include <hyperspacehashing/search.h>

// HyperDex
#include <hyperdex/configuration.h>
#include <hyperdex/coordinatorlink.h>
#include <hyperdex/network_constants.h>

// HyperClient
#include <hyperclient/client.h>

namespace hyperclient
{

class channel
{
    public:
        channel(const hyperdex::instance& inst);
        ~channel() throw ();

    public:
        po6::net::socket soc;
        uint64_t nonce;
        hyperdex::entityid id;

    private:
        friend class e::intrusive_ptr<channel>;

    private:
        void inc() { ++m_ref; }
        void dec() { if (--m_ref == 0) delete this; }

    private:
        size_t m_ref;
};

class pending
{
    public:
        pending();
        virtual ~pending() throw ();

    public:
        virtual void result(returncode ret) = 0;
        virtual e::intrusive_ptr<pending> result(hyperdex::network_msgtype msg_type,
                                                 const e::buffer& msg,
                                                 bool* called_back) = 0;

    public:
        e::intrusive_ptr<channel> chan;
        hyperdex::entityid ent;
        hyperdex::instance inst;
        uint64_t nonce;
        bool reconfigured;

    private:
        friend class e::intrusive_ptr<pending>;

    private:
        void inc() { ++m_ref; }
        void dec() { if (--m_ref == 0) delete this; }

    private:
        size_t m_ref;
};

class pending_get : public pending
{
    public:
        pending_get(std::tr1::function<void (returncode, const std::vector<e::buffer>&)> callback);
        virtual ~pending_get() throw ();

    public:
        virtual void result(returncode ret);
        virtual e::intrusive_ptr<pending> result(hyperdex::network_msgtype msg_type,
                                                 const e::buffer& msg,
                                                 bool* called_back);

    private:
        std::tr1::function<void (returncode, const std::vector<e::buffer>&)> m_callback;
};

class pending_mutate : public pending
{
    public:
        pending_mutate(hyperdex::network_msgtype expected,
                       std::tr1::function<void (returncode)> callback);
        virtual ~pending_mutate() throw ();

    public:
        virtual void result(returncode ret);
        virtual e::intrusive_ptr<pending> result(hyperdex::network_msgtype msg_type,
                                                 const e::buffer& msg,
                                                 bool* called_back);

    private:
        hyperdex::network_msgtype m_expected;
        std::tr1::function<void (returncode)> m_callback;
};

class pending_search : public pending
{
    public:
        pending_search(uint64_t searchid,
                       client* aci,
                       std::tr1::function<void (returncode,
                                                const e::buffer&,
                                                const std::vector<e::buffer>&)> callback);
        virtual ~pending_search() throw ();

    public:
        virtual void result(returncode ret);
        virtual e::intrusive_ptr<pending> result(hyperdex::network_msgtype msg_type,
                                                 const e::buffer& msg,
                                                 bool* called_back);

    private:
        pending_search(const pending_search&);

    public:
        pending_search& operator = (const pending_search&);

    private:
        uint64_t m_searchid;
        client* m_aci;
        std::tr1::function<void (returncode,
                                 const e::buffer&,
                                 const std::vector<e::buffer>&)> m_callback;
};

class client :: priv
{
    public:
        priv(const po6::net::location& coordinator);

    public:
        void add_reqrep(const std::string&, const e::buffer& key,
                        hyperdex::network_msgtype send_type,
                        const e::buffer& send_msg, e::intrusive_ptr<pending> op);
        bool send(e::intrusive_ptr<channel> chan,
                  e::intrusive_ptr<pending> op,
                  const hyperdex::entityid& entity,
                  const hyperdex::instance& inst,
                  uint64_t nonce,
                  hyperdex::network_msgtype send_type,
                  const e::buffer& send_msg);

    public:
        bool initialized;
        hyperdex::coordinatorlink coord;
        hyperdex::configuration config;
        std::map<hyperdex::instance, e::intrusive_ptr<channel> > channels;
        std::deque<e::intrusive_ptr<pending> > requests;
        uint64_t searchid;
};

} // hyperclient

hyperclient :: client :: priv :: priv(const po6::net::location& coordinator)
    : initialized(false)
    , coord(coordinator)
    , config()
    , channels()
    , requests()
    , searchid(1)
{
    coord.set_announce("client");
}

void
hyperclient :: client :: priv :: add_reqrep(const std::string& space,
                                            const e::buffer& key,
                                            hyperdex::network_msgtype send_type,
                                            const e::buffer& send_msg,
                                            e::intrusive_ptr<pending> op)
{
    hyperdex::spaceid si = config.lookup_spaceid(space);

    if (si == hyperdex::configuration::NULLSPACE)
    {
        op->result(NOTASPACE);
        return;
    }

    // Figure out who to talk with.
    hyperdex::entityid dst_ent;
    hyperdex::instance dst_inst;

    if (!config.point_leader_entity(si, key, &dst_ent, &dst_inst))
    {
        op->result(CONNECTFAIL);
        return;
    }

    e::intrusive_ptr<channel> chan = channels[dst_inst];

    if (!chan)
    {
        try
        {
            channels[dst_inst] = chan = new channel(dst_inst);
        }
        catch (po6::error& e)
        {
            op->result(CONNECTFAIL);
            return;
        }
    }

    uint64_t nonce = chan->nonce;
    ++chan->nonce;
    op->chan = chan;
    op->ent = dst_ent;
    op->inst = dst_inst;
    op->nonce = nonce;
    requests.push_back(op);

    if (!send(chan, op, dst_ent, dst_inst, nonce, send_type, send_msg))
    {
        requests.pop_back();
    }
}

bool
hyperclient :: client :: priv :: send(e::intrusive_ptr<channel> chan,
                                      e::intrusive_ptr<pending> op,
                                      const hyperdex::entityid& ent,
                                      const hyperdex::instance& inst,
                                      uint64_t nonce,
                                      hyperdex::network_msgtype send_type,
                                      const e::buffer& send_msg)
{
    const uint8_t type = static_cast<uint8_t>(send_type);
    const uint16_t fromver = 0;
    const uint16_t tover = inst.inbound_version;
    const hyperdex::entityid& from(chan->id);
    const hyperdex::entityid& to(ent);
    const uint32_t size = sizeof(type) + sizeof(fromver)
                        + sizeof(tover) + hyperdex::entityid::SERIALIZEDSIZE * 2
                        + sizeof(nonce) + send_msg.size();
    e::buffer packed(size);
    packed.pack() << size << type
                  << fromver << tover
                  << from << to
                  << nonce;
    packed += send_msg;

    try
    {
        chan->soc.xsend(packed.get(), packed.size(), MSG_NOSIGNAL);
    }
    catch (po6::error& e)
    {
        channels.erase(inst);
        op->result(DISCONNECT);
        return false;
    }

    return true;
}

hyperclient :: client :: client(po6::net::location coordinator)
    : p(new priv(coordinator))
{
}

hyperclient :: client :: ~client() throw ()
{
}

hyperclient::returncode
hyperclient :: client :: connect()
{
    switch (p->coord.connect())
    {
        case hyperdex::coordinatorlink::SUCCESS:
            break;
        case hyperdex::coordinatorlink::CONNECTFAIL:
            return COORDFAIL;
        case hyperdex::coordinatorlink::DISCONNECT:
        case hyperdex::coordinatorlink::SHUTDOWN:
        case hyperdex::coordinatorlink::LOGICERROR:
        default:
            return LOGICERROR;
    }

    while (true)
    {
        switch (p->coord.loop(1, -1))
        {
            case hyperdex::coordinatorlink::SUCCESS:
                break;
            case hyperdex::coordinatorlink::CONNECTFAIL:
                return COORDFAIL;
            case hyperdex::coordinatorlink::DISCONNECT:
                return COORDFAIL;
            case hyperdex::coordinatorlink::SHUTDOWN:
            case hyperdex::coordinatorlink::LOGICERROR:
            default:
                return LOGICERROR;
        }

        if (p->coord.unacknowledged())
        {
            p->config = p->coord.config();
            p->coord.acknowledge();
            break;
        }
    }

    return SUCCESS;
}

void
hyperclient :: client :: get(const std::string& space,
                             const e::buffer& key,
                             std::tr1::function<void (returncode, const std::vector<e::buffer>&)> callback)
{
    e::intrusive_ptr<pending> op = new pending_get(callback);
    p->add_reqrep(space, key, hyperdex::REQ_GET, key, op);
}

void
hyperclient :: client :: get(const std::string& space,
                             const e::buffer& key,
                             returncode* ret, std::vector<e::buffer>* value)
{
    using std::tr1::bind;
    using std::tr1::placeholders::_1;
    using std::tr1::placeholders::_2;
    get(space, key, bind(&client::inner_get, this, ret, value, _1, _2));
}

void
hyperclient :: client :: put(const std::string& space,
                             const e::buffer& key,
                             const std::vector<e::buffer>& value,
                             std::tr1::function<void (returncode)> callback)
{
    e::buffer msg;
    msg.pack() << key << value;
    e::intrusive_ptr<pending> op = new pending_mutate(hyperdex::RESP_PUT, callback);
    p->add_reqrep(space, key, hyperdex::REQ_PUT, msg, op);
}

void
hyperclient :: client :: put(const std::string& space,
                             const e::buffer& key,
                             const std::vector<e::buffer>& value,
                             returncode* ret)
{
    using std::tr1::bind;
    using std::tr1::placeholders::_1;
    put(space, key, value, bind(&client::inner_mutate, this, ret, _1));
}

void
hyperclient :: client :: del(const std::string& space,
                             const e::buffer& key,
                             std::tr1::function<void (returncode)> callback)
{
    e::intrusive_ptr<pending> op = new pending_mutate(hyperdex::RESP_DEL, callback);
    p->add_reqrep(space, key, hyperdex::REQ_DEL, key, op);
}

void
hyperclient :: client :: del(const std::string& space,
                             const e::buffer& key,
                             returncode* ret)
{
    using std::tr1::bind;
    using std::tr1::placeholders::_1;
    del(space, key, bind(&client::inner_mutate, this, ret, _1));
}

void
hyperclient :: client :: update(const std::string& space,
                                const e::buffer& key,
                                const std::map<std::string, e::buffer>& value,
                                std::tr1::function<void (returncode)> callback)
{
    hyperdex::spaceid si = p->config.lookup_spaceid(space);

    if (si == hyperdex::configuration::NULLSPACE)
    {
        callback(NOTASPACE);
        return;
    }

    std::vector<std::string> dimension_names = p->config.lookup_space_dimensions(si);
    assert(dimension_names.size() > 0);

    e::bitfield bits(dimension_names.size() - 1);
    std::vector<e::buffer> realvalue(dimension_names.size() - 1);
    std::set<std::string> seen;

    for (size_t i = 1; i < dimension_names.size(); ++i)
    {
        std::map<std::string, e::buffer>::const_iterator valiter;
        valiter = value.find(dimension_names[i]);

        if (valiter == value.end())
        {
            bits.unset(i - 1);
        }
        else
        {
            seen.insert(valiter->first);
            bits.set(i - 1);
            realvalue[i - 1] = valiter->second;
        }
    }

    for (std::map<std::string, e::buffer>::const_iterator i = value.begin();
            i != value.end(); ++i)
    {
        if (seen.find(i->first) == seen.end())
        {
            callback(BADDIMENSION);
            return;
        }
    }

    e::buffer msg;
    msg.pack() << key << bits << realvalue;
    e::intrusive_ptr<pending> op = new pending_mutate(hyperdex::RESP_UPDATE, callback);
    p->add_reqrep(space, key, hyperdex::REQ_UPDATE, msg, op);
}

void
hyperclient :: client :: update(const std::string& space,
                                const e::buffer& key,
                                const std::map<std::string, e::buffer>& value,
                                returncode* ret)
{
    using std::tr1::bind;
    using std::tr1::placeholders::_1;
    update(space, key, value, bind(&client::inner_mutate, this, ret, _1));
}

void
hyperclient :: client :: search(const std::string& space,
                                const std::map<std::string, e::buffer>& params,
                                std::tr1::function<void (returncode,
                                                         const e::buffer&,
                                                         const std::vector<e::buffer>&)> callback)
{
    std::map<std::string, std::pair<uint64_t, uint64_t> > range;
    search(space, params, range, callback, UINT16_MAX);
}

void
hyperclient :: client :: search(const std::string& space,
                                const std::map<std::string, std::pair<uint64_t, uint64_t> >& params,
                                std::tr1::function<void (returncode,
                                                         const e::buffer&,
                                                         const std::vector<e::buffer>&)> callback)
{
    std::map<std::string, e::buffer> equality;
    search(space, equality, params, callback, UINT16_MAX);
}

void
hyperclient :: client :: search(const std::string& space,
                                const std::map<std::string, e::buffer>& equality,
                                const std::map<std::string, std::pair<uint64_t, uint64_t> >& range,
                                std::tr1::function<void (returncode,
                                                         const e::buffer&,
                                                         const std::vector<e::buffer>&)> callback)
{
    return search(space, equality, range, callback, UINT16_MAX);
}

void
hyperclient :: client :: search(const std::string& space,
                                const std::map<std::string, e::buffer>& equality,
                                const std::map<std::string, std::pair<uint64_t, uint64_t> >& range,
                                std::tr1::function<void (returncode,
                                                         const e::buffer&,
                                                         const std::vector<e::buffer>&)> callback,
                                uint16_t subspace_hint)
{
    e::buffer pseudokey;
    std::vector<e::buffer> pseudovalue;

    // Lookup the space
    hyperdex::spaceid si = p->config.lookup_spaceid(space);

    if (si == hyperdex::configuration::NULLSPACE)
    {
        callback(NOTASPACE, pseudokey, pseudovalue);
        return;
    }

    std::set<std::string> specified_dimensions;
    std::vector<std::string> dimension_names = p->config.lookup_space_dimensions(si);
    assert(dimension_names.size() > 0);

    // Create a search object from the search terms.
    hyperspacehashing::search s(dimension_names.size());

    for (std::map<std::string, e::buffer>::const_iterator param = equality.begin();
            param != equality.end(); ++param)
    {
        std::vector<std::string>::const_iterator dim;
        dim = std::find(dimension_names.begin(), dimension_names.end(), param->first);

        if (dim == dimension_names.begin() || dim == dimension_names.end())
        {
            callback(BADSEARCH, pseudokey, pseudovalue);
            return;
        }

        specified_dimensions.insert(param->first);
        s.equality_set((dim - dimension_names.begin()), param->second);
    }

    for (std::map<std::string, std::pair<uint64_t, uint64_t> >::const_iterator param = range.begin();
            param != range.end(); ++param)
    {
        if (specified_dimensions.find(param->first) != specified_dimensions.end())
        {
            callback(BADSEARCH, pseudokey, pseudovalue);
            return;
        }

        std::vector<std::string>::const_iterator dim;
        dim = std::find(dimension_names.begin(), dimension_names.end(), param->first);

        if (dim == dimension_names.end())
        {
            callback(BADSEARCH, pseudokey, pseudovalue);
            return;
        }

        s.range_set((dim - dimension_names.begin()), param->second.first, param->second.second);
    }

    // Get the hosts that match our search terms.
    std::map<hyperdex::entityid, hyperdex::instance> search_entities;

    if (subspace_hint == UINT16_MAX)
    {
        search_entities = p->config.search_entities(si, s);
    }
    else
    {
        search_entities = p->config.search_entities(hyperdex::subspaceid(si, subspace_hint), s);
    }

    uint64_t searchid = p->searchid;
    ++p->searchid;
    e::buffer req;
    req.pack() << searchid << s;

    for (std::map<hyperdex::entityid, hyperdex::instance>::const_iterator ent_inst = search_entities.begin();
            ent_inst != search_entities.end(); ++ ent_inst)
    {
        e::intrusive_ptr<pending> op = new pending_search(searchid, this, callback);
        e::intrusive_ptr<channel> chan = p->channels[ent_inst->second];
        p->requests.push_back(op);

        if (!chan)
        {
            try
            {
                p->channels[ent_inst->second] = chan = new channel(ent_inst->second);
            }
            catch (po6::error& e)
            {
                op->result(CONNECTFAIL);
                p->requests.pop_back();
                continue;
            }
        }

        uint64_t nonce = chan->nonce;
        ++chan->nonce;
        op->chan = chan;
        op->ent = ent_inst->first;
        op->inst = ent_inst->second;
        op->nonce = nonce;

        if (!p->send(chan, op, op->ent, op->inst, nonce, hyperdex::REQ_SEARCH_START, req))
        {
            p->requests.pop_back();
        }
    }
}

size_t
hyperclient :: client :: outstanding()
{
    size_t ret = 0;
    std::deque<e::intrusive_ptr<pending> >::iterator req = p->requests.begin();

    while (req != p->requests.end())
    {
        if (*req)
        {
            ++ret;
            ++req;
        }
        else
        {
            req = p->requests.erase(req);
        }
    }

    return ret;
}

hyperclient::returncode
hyperclient :: client :: flush(int timeout)
{
    int original_timeout = timeout;
    e::stopwatch stopw;
    stopw.start();

    while (!p->requests.empty())
    {
        returncode ret = flush_one(timeout);

        if (ret != SUCCESS && ret != TIMEOUT)
        {
            return ret;
        }

        if (timeout >= 0)
        {
            timeout = original_timeout - stopw.peek_ms();

            if (timeout < 0)
            {
                ret = TIMEOUT;
            }
        }

        if (ret == TIMEOUT)
        {
            for (std::deque<e::intrusive_ptr<pending> >::iterator req = p->requests.begin();
                    req != p->requests.end(); ++req)
            {
                if (*req)
                {
                    (*req)->result(TIMEOUT);
                }
            }

            p->requests.clear();
            return TIMEOUT;
        }
    }

    return SUCCESS;
}

hyperclient::returncode
hyperclient :: client :: flush_one(int timeout)
{
    while (!p->requests.empty())
    {
        if (!p->requests.front())
        {
            p->requests.pop_front();
            continue;
        }

        for (int i = 0; i < 7 && !p->coord.connected(); ++i)
        {
            switch (p->coord.connect())
            {
                case hyperdex::coordinatorlink::SUCCESS:
                    break;
                case hyperdex::coordinatorlink::CONNECTFAIL:
                case hyperdex::coordinatorlink::DISCONNECT:

                    if (i == 6)
                    {
                        return COORDFAIL;
                    }

                    break;
                case hyperdex::coordinatorlink::SHUTDOWN:
                case hyperdex::coordinatorlink::LOGICERROR:
                default:

                    if (i == 6)
                    {
                        return LOGICERROR;
                    }

                    break;
            }
        }

        size_t num_pfds = p->requests.size();
        std::vector<pollfd> pfds(num_pfds + 1);

        for (size_t i = 0; i < num_pfds; ++i)
        {
            if (p->requests[i])
            {
                pfds[i].fd = p->requests[i]->chan->soc.get();
            }
            else
            {
                pfds[i].fd = -1;
            }

            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }

        pfds[num_pfds] = p->coord.pfd();
        pfds[num_pfds].revents = 0;
        int polled = poll(&pfds.front(), num_pfds + 1, timeout);

        if (polled < 0)
        {
            return LOGICERROR;
        }

        if (polled == 0)
        {
            while (!p->requests.empty())
            {
                if (!p->requests.front())
                {
                    p->requests.pop_front();
                    continue;
                }

                p->requests.front()->result(TIMEOUT);
                p->requests.pop_front();
                break;
            }

            return TIMEOUT;
        }

        if (poll(&pfds.front(), num_pfds + 1, -1) < 0)
        {
            return LOGICERROR;
        }

        if (pfds[num_pfds].revents != 0)
        {
            switch (p->coord.loop(1, 0))
            {
                case hyperdex::coordinatorlink::SUCCESS:
                    break;
                case hyperdex::coordinatorlink::CONNECTFAIL:
                case hyperdex::coordinatorlink::DISCONNECT:
                    return COORDFAIL;
                case hyperdex::coordinatorlink::SHUTDOWN:
                case hyperdex::coordinatorlink::LOGICERROR:
                default:
                    return LOGICERROR;
            }
        }

        if (p->coord.unacknowledged())
        {
            p->config = p->coord.config();
            p->coord.acknowledge();

            for (std::deque<e::intrusive_ptr<pending> >::iterator i = p->requests.begin();
                    i != p->requests.end(); ++i)
            {
                if (*i && p->config.instancefor((*i)->ent) != (*i)->inst)
                {
                    (*i)->reconfigured = true;
                }
            }

            continue;
        }

        for (size_t i = 0; i < num_pfds; ++i)
        {
            if (!p->requests[i])
            {
                continue;
            }

            if ((pfds[i].revents & POLLHUP) || (pfds[i].revents & POLLERR))
            {
                p->requests[i]->chan->soc.close();
                p->channels.erase(p->requests[i]->inst);
                p->requests[i]->result(DISCONNECT);
                p->requests[i] = NULL;
                return SUCCESS;
            }

            if (p->requests[i]->reconfigured)
            {
                p->requests[i]->result(RECONFIGURE);
                p->requests[i] = NULL;
                return SUCCESS;
            }

            if (!(pfds[i].revents & POLLIN))
            {
                continue;
            }

            e::intrusive_ptr<channel> chan = p->requests[i]->chan;

            if (chan->soc.get() < 0)
            {
                p->requests[i]->result(DISCONNECT);
                p->requests[i] = NULL;
                return SUCCESS;
            }

            try
            {
                uint32_t size;

                if (recv(chan->soc.get(), &size, 4, MSG_DONTWAIT|MSG_PEEK) != 4)
                {
                    continue;
                }

                size = be32toh(size);
                size += sizeof(uint32_t);
                e::buffer response(size);

                if (xread(&chan->soc, &response, size) < size)
                {
                    chan->soc.close();
                    p->channels.erase(p->requests[i]->inst);
                    p->requests[i]->result(DISCONNECT);
                    p->requests[i] = NULL;
                    return SUCCESS;
                }

                uint32_t nop;
                uint8_t type_num;
                uint16_t fromver;
                uint16_t tover;
                hyperdex::entityid from;
                hyperdex::entityid to;
                uint64_t nonce;
                e::unpacker up(response.unpack());
                up >> nop >> type_num >> fromver >> tover >> from >> to >> nonce;
                hyperdex::network_msgtype msg_type = static_cast<hyperdex::network_msgtype>(type_num);

                if (chan->id == hyperdex::entityid(hyperdex::configuration::CLIENTSPACE))
                {
                    chan->id = to;
                }

                e::buffer msg;
                up.leftovers(&msg);

                for (std::deque<e::intrusive_ptr<pending> >::iterator req = p->requests.begin();
                        req != p->requests.end(); ++req)
                {
                    if (*req &&
                        chan == (*req)->chan &&
                        fromver == (*req)->inst.inbound_version &&
                        tover == 0 &&
                        from == (*req)->ent &&
                        to == chan->id &&
                        nonce == (*req)->nonce)
                    {
                        bool called_back = false;
                        *req = (*req)->result(msg_type, msg, &called_back);

                        if (called_back)
                        {
                            return SUCCESS;
                        }
                    }
                }
            }
            catch (po6::error& e)
            {
                p->requests[i]->chan->soc.close();
                p->channels.erase(p->requests[i]->inst);
                p->requests[i]->result(DISCONNECT);
                p->requests[i] = NULL;
                return SUCCESS;
            }
            catch (std::out_of_range& e)
            {
                p->requests[i]->chan->soc.close();
                p->channels.erase(p->requests[i]->inst);
                p->requests[i]->result(DISCONNECT);
                p->requests[i] = NULL;
                return SUCCESS;
            }
        }

        while (!p->requests.empty() && !p->requests.front())
        {
            p->requests.pop_front();
        }
    }

    return SUCCESS;
}

hyperclient::returncode
hyperclient :: client :: get(const std::string& space, const e::buffer& key,
                             std::map<std::string, e::buffer>* value)
{
    hyperdex::spaceid si = p->config.lookup_spaceid(space);

    if (si == hyperdex::configuration::NULLSPACE)
    {
        return NOTASPACE;
    }

    std::vector<std::string> dimension_names = p->config.lookup_space_dimensions(si);
    assert(dimension_names.size() > 0);
    hyperclient::returncode ret;
    std::vector<e::buffer> realvalue;
    get(space, key, &ret, &realvalue);
    flush(-1);

    if (ret != SUCCESS)
    {
        return ret;
    }

    if (realvalue.size() + 1 != dimension_names.size())
    {
        return SERVERERROR;
    }

    value->clear();

    for (size_t i = 1; i < dimension_names.size(); ++i)
    {
        value->insert(std::make_pair(dimension_names[i], realvalue[i - 1]));
    }

    return SUCCESS;
}

hyperclient::returncode
hyperclient :: client :: put(const std::string& space, const e::buffer& key,
                             const std::map<std::string, e::buffer>& value)
{
    hyperclient::returncode ret;
    update(space, key, value, &ret);
    flush(-1);
    return ret;
}

hyperclient::returncode
hyperclient :: client :: del(const std::string& space, const e::buffer& key)
{
    hyperclient::returncode ret;
    del(space, key, &ret);
    flush(-1);
    return ret;
}

hyperclient::returncode
hyperclient :: client :: search(const std::string& space,
                                const std::map<std::string, e::buffer>& equality,
                                const std::map<std::string, std::pair<uint64_t, uint64_t> >& range,
                                std::vector<std::map<std::string, e::buffer> >* results)
{
    hyperdex::spaceid si = p->config.lookup_spaceid(space);

    if (si == hyperdex::configuration::NULLSPACE)
    {
        return NOTASPACE;
    }

    std::vector<std::string> dimension_names = p->config.lookup_space_dimensions(si);
    assert(dimension_names.size() > 0);
    hyperclient::returncode ret = NOTFOUND;
    e::buffer key;
    std::vector<e::buffer> value;
    results->clear();

    using std::tr1::bind;
    using std::tr1::placeholders::_1;
    using std::tr1::placeholders::_2;
    using std::tr1::placeholders::_3;
    search(space, equality, range, bind(&client::inner_search, this, &ret, &key, &value, _1, _2, _3));

    while (outstanding() > 0)
    {
        flush_one(-1);

        if (ret == SUCCESS && value.size() + 1 == dimension_names.size())
        {
            std::map<std::string, e::buffer> namedvalue;
            namedvalue.insert(std::make_pair(dimension_names[0], key));

            for (size_t i = 1; i < dimension_names.size(); ++i)
            {
                namedvalue.insert(std::make_pair(dimension_names[i], value[i - 1]));
            }

            results->push_back(namedvalue);
        }
        else if (ret == SUCCESS)
        {
            return SERVERERROR;
        }
    }

    return ret;
}

void
hyperclient :: client :: inner_get(returncode* ret,
                                   std::vector<e::buffer>* value,
                                   returncode _ret,
                                   const std::vector<e::buffer>& _value)
{
    *ret = _ret;
    *value = _value;
}

void
hyperclient :: client :: inner_mutate(returncode* ret, returncode _ret)
{
    *ret = _ret;
}

void
hyperclient :: client :: inner_search(returncode* ret,
                                      e::buffer* key,
                                      std::vector<e::buffer>* value,
                                      returncode _ret,
                                      const e::buffer& _key,
                                      const std::vector<e::buffer>& _value)
{
    if (_ret == SUCCESS)
    {
        *key = _key;
        *value = _value;
    }

    *ret = _ret;
}

hyperclient :: channel :: channel(const hyperdex::instance& inst)
    : soc(inst.inbound.address.family(), SOCK_STREAM, IPPROTO_TCP)
    , nonce(1)
    , id(hyperdex::configuration::CLIENTSPACE)
    , m_ref(0)
{
    soc.connect(inst.inbound);
    soc.tcp_nodelay(true);
}

hyperclient :: channel :: ~channel() throw ()
{
}

hyperclient :: pending :: pending()
    : chan()
    , ent()
    , inst()
    , nonce()
    , reconfigured(false)
    , m_ref(0)
{
}

hyperclient :: pending :: ~pending() throw ()
{
}

hyperclient :: pending_get :: pending_get(std::tr1::function<void (returncode, const std::vector<e::buffer>&)> callback)
    : m_callback(callback)
{
}

hyperclient :: pending_get :: ~pending_get() throw ()
{
}

void
hyperclient :: pending_get :: result(returncode ret)
{
    std::vector<e::buffer> res;
    m_callback(ret, res);
}

e::intrusive_ptr<hyperclient::pending>
hyperclient :: pending_get :: result(hyperdex::network_msgtype msg_type,
                                     const e::buffer& msg,
                                     bool* called_back)
{
    *called_back = true;
    std::vector<e::buffer> value;

    if (msg_type != hyperdex::RESP_GET)
    {
        m_callback(SERVERERROR, value);
        return NULL;
    }

    try
    {
        e::unpacker up(msg);
        uint16_t response;
        up >> response;

        switch (static_cast<hyperdex::network_returncode>(response))
        {
            case hyperdex::NET_SUCCESS:
                up >> value;
                m_callback(SUCCESS, value);
                break;
            case hyperdex::NET_NOTFOUND:
                m_callback(NOTFOUND, value);
                break;
            case hyperdex::NET_WRONGARITY:
                m_callback(WRONGARITY, value);
                break;
            case hyperdex::NET_NOTUS:
                m_callback(LOGICERROR, value);
                break;
            case hyperdex::NET_SERVERERROR:
                m_callback(SERVERERROR, value);
                break;
            default:
                m_callback(SERVERERROR, value);
                break;
        }
    }
    catch (std::out_of_range& e)
    {
        m_callback(SERVERERROR, value);
    }

    return NULL;
}

hyperclient :: pending_mutate :: pending_mutate(hyperdex::network_msgtype expected,
                                                std::tr1::function<void (returncode)> callback)
    : m_expected(expected)
    , m_callback(callback)
{
}

hyperclient :: pending_mutate :: ~pending_mutate() throw ()
{
}

void
hyperclient :: pending_mutate :: result(returncode ret)
{
    m_callback(ret);
}

e::intrusive_ptr<hyperclient::pending>
hyperclient :: pending_mutate :: result(hyperdex::network_msgtype msg_type,
                                        const e::buffer& msg,
                                        bool* called_back)
{
    *called_back = true;

    if (msg_type != m_expected)
    {
        m_callback(SERVERERROR);
        return NULL;
    }

    try
    {
        e::unpacker up(msg);
        uint16_t response;
        up >> response;

        switch (static_cast<hyperdex::network_returncode>(response))
        {
            case hyperdex::NET_SUCCESS:
                m_callback(SUCCESS);
                break;
            case hyperdex::NET_NOTFOUND:
                m_callback(NOTFOUND);
                break;
            case hyperdex::NET_WRONGARITY:
                m_callback(WRONGARITY);
                break;
            case hyperdex::NET_NOTUS:
                m_callback(LOGICERROR);
                break;
            case hyperdex::NET_SERVERERROR:
                m_callback(SERVERERROR);
                break;
            default:
                m_callback(SERVERERROR);
                break;
        }
    }
    catch (std::out_of_range& e)
    {
        m_callback(SERVERERROR);
    }

    return NULL;
}

hyperclient :: pending_search :: pending_search(uint64_t searchid,
                                                client* aci,
                                                std::tr1::function<void (returncode,
                                                                         const e::buffer&,
                                                                         const std::vector<e::buffer>&)> callback)
    : m_searchid(searchid)
    , m_aci(aci)
    , m_callback(callback)
{
}

hyperclient :: pending_search :: ~pending_search() throw ()
{
}

void
hyperclient :: pending_search :: result(returncode ret)
{
    e::buffer pseudokey;
    std::vector<e::buffer> pseudovalue;
    m_callback(ret, pseudokey, pseudovalue);
}

e::intrusive_ptr<hyperclient::pending>
hyperclient :: pending_search :: result(hyperdex::network_msgtype msg_type,
                                        const e::buffer& msg,
                                        bool* called_back)
{
    *called_back = true;
    e::buffer key;
    std::vector<e::buffer> value;

    if (msg_type == hyperdex::RESP_SEARCH_ITEM)
    {
        try
        {
            msg.unpack() >> key >> value;
            nonce = chan->nonce;
            ++chan->nonce;
            e::buffer req;
            req.pack() << m_searchid;

            if (m_aci->p->send(chan, this, ent, inst, nonce, hyperdex::REQ_SEARCH_NEXT, req))
            {
                m_callback(SUCCESS, key, value);
                return this;
            }
            else
            {
                return NULL;
            }
        }
        catch (std::out_of_range& e)
        {
            m_callback(SERVERERROR, key, value);
            return NULL;
        }
    }
    else if (msg_type == hyperdex::RESP_SEARCH_DONE)
    {
        *called_back = false;
        return NULL;
    }
    else
    {
        m_callback(SERVERERROR, key, value);
        return NULL;
    }
}
