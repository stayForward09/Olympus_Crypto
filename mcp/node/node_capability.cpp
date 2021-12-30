#include "node_capability.hpp"
#include <fstream>

mcp::node_capability::node_capability(
	boost::asio::io_service& io_service_a, mcp::block_store& store_a,
	mcp::fast_steady_clock& steady_clock_a, std::shared_ptr<mcp::block_cache> cache_a,
	std::shared_ptr<mcp::async_task> async_task_a, std::shared_ptr<mcp::block_arrival> block_arrival_a
	)
    :icapability(p2p::capability_desc("mcp", 0), (unsigned)mcp::sub_packet_type::packet_count),
	m_io_service(io_service_a),
	m_store(store_a),
	m_steady_clock(steady_clock_a),
	m_cache(cache_a),
	m_async_task(async_task_a),
	m_block_arrival(block_arrival_a),
    m_stopped(false),
    m_genesis(0),
    m_pcapability_metrics(std::make_shared<mcp::capability_metrics>()),
	m_requesting(*this)
{
	m_request_timer = std::make_unique<ba::deadline_timer>(m_io_service);
}

void mcp::node_capability::stop()
{
	boost::system::error_code ec;
	if (m_request_timer)
		m_request_timer->cancel(ec);

	m_stopped = true;
}

void mcp::node_capability::on_connect(std::shared_ptr<p2p::peer> peer_a, unsigned const & offset)
{
    auto  node_id = peer_a->remote_node_id();
    if (m_genesis == 0)
    {
        mcp::db::db_transaction transaction(m_store.create_transaction());
        bool error = m_store.genesis_hash_get(transaction, m_genesis);
        assert_x(!error);
    }
    
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        m_wait_confirm_remote_node[node_id] = mcp::local_remote_ack_hello();
        m_peers.insert(std::make_pair(node_id, mcp::peer_info(peer_a, offset)));
        m_node_id_cap_metrics[node_id] = std::make_shared<mcp::capability_metrics>();

        //one peer one timer
        m_sync_peer_info_request_timer[node_id] = std::make_unique<ba::deadline_timer>(m_io_service);

    }
    send_hello_info_request(node_id);

    {    
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        if (m_sync_peer_info_request_timer.count(node_id) > 0)
        {
            m_sync_peer_info_request_timer[node_id]->expires_from_now(boost::posix_time::seconds(180));
            m_sync_peer_info_request_timer[node_id]->async_wait([this, node_id](boost::system::error_code const & error)
            {
                if (!error)
                    timeout_for_ack_hello(node_id);
            });
        }
    }

	if (!m_request_associng.test_and_set())
	{
		request_block_timeout();
	}
}

void mcp::node_capability::on_disconnect(std::shared_ptr<p2p::peer> peer_a)
{
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        m_peers.erase(peer_a->remote_node_id());

        m_node_id_cap_metrics.erase(peer_a->remote_node_id());

        if (m_wait_confirm_remote_node.count(peer_a->remote_node_id()) > 0)
        {
            m_wait_confirm_remote_node.erase(peer_a->remote_node_id());
        }
        if (m_sync_peer_info_request_timer.count(peer_a->remote_node_id()) > 0)
        {
            m_sync_peer_info_request_timer.erase(peer_a->remote_node_id());
        }
        
    }
   
}

void mcp::node_capability::request_block_timeout()
{
	uint64_t now = m_steady_clock.now_since_epoch();

	std::list<mcp::requesting_item> requests;
	{
		std::lock_guard<std::mutex> lock(m_requesting_lock);
		requests = m_requesting.clear_by_time(now);
	}
	mcp::db::db_transaction transaction(m_store.create_transaction());
	for (auto const& it : requests)
	{
		bool exist = false;
		if (m_block_processor->unhandle->exists(it.m_request_hash))
		{
			exist = true;
		}
		else
		{
			if (mcp::block_type::dag == it.m_block_type)
			{
				if (m_store.block_exists(transaction, it.m_request_hash))
					exist = true;
			}
			else
			{
				if (m_cache->unlink_block_exists(transaction, it.m_request_hash))
					exist = true;
				else if (m_cache->block_exists(transaction, it.m_request_hash))
					exist = true;
			}
		}
		
		if (!exist)
		{
			auto hash(std::make_shared<mcp::block_hash>(it.m_request_hash));
			mcp::joint_request_item request_item(it.m_node_id, hash, it.m_block_type, mcp::requesting_block_cause::new_unknown);
			m_sync->request_new_missing_joints(request_item, now, true);
		}
		else
		{
			std::lock_guard<std::mutex> lock(m_requesting_lock);
			m_requesting.erase(it.m_request_hash);
		}
	}

	m_request_timer->expires_from_now(boost::posix_time::seconds(20));
	m_request_timer->async_wait([this](boost::system::error_code const & error)
	{
		if (!error)
			request_block_timeout();
	});
}

void mcp::node_capability::timeout_for_ack_hello(p2p::node_id node_id_a)
{
    std::shared_ptr<p2p::peer> p;
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        if (m_wait_confirm_remote_node.count(node_id_a) > 0 && m_peers.count(node_id_a) > 0)
        {
            mcp::peer_info &pi(m_peers.at(node_id_a));
            p = pi.try_lock_peer();
            if (!p)
            {
                //weak_ptr use_count=0,peer drop but we dont know.
                m_wait_confirm_remote_node.erase(node_id_a);
                m_peers.erase(node_id_a);
                m_sync_peer_info_request_timer.erase(node_id_a);
            }
        }
    }
    if (p)
    {
        p->disconnect(p2p::disconnect_reason::tcp_error);
    }
}


bool mcp::node_capability::is_peer_exsist(p2p::node_id const & id)
{
    return (m_peers.count(id) > 0);
}

void mcp::node_capability::confirm_remote_connect(p2p::node_id node_id_a)
{
    {
        std::lock_guard<std::mutex> lock(m_peers_mutex);
        m_wait_confirm_remote_node.erase(node_id_a);
        if (m_sync_peer_info_request_timer.count(node_id_a) > 0)
        {
            m_sync_peer_info_request_timer[node_id_a]->expires_from_now(boost::posix_time::milliseconds(1000));
            m_sync_peer_info_request_timer[node_id_a]->async_wait([this, node_id_a](boost::system::error_code const & error)
            {
				if (!error)
				{
					m_sync->send_peer_info_request(node_id_a);
				}
            });
        }

    }
}

bool mcp::node_capability::is_local_remote_ack_ok_hello(p2p::node_id node_id_a)
{
    bool result(false);
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    if (m_wait_confirm_remote_node.count(node_id_a) > 0)
    {
        result = m_wait_confirm_remote_node[node_id_a].l_r_result && m_wait_confirm_remote_node[node_id_a].r_l_result;
    }
    return result;
}

bool mcp::node_capability::check_remotenode_hello(mcp::block_hash const & block_hash_a)
{
    return block_hash_a == m_genesis;
}

inline bool mcp::node_capability::is_remote_in_waitting_hello(p2p::node_id node_id_a)
{
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    return m_wait_confirm_remote_node.count(node_id_a);
}

void mcp::node_capability::set_local_ack_hello(p2p::node_id node_id_a, bool local)
{
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    if (m_wait_confirm_remote_node.count(node_id_a) > 0)
    {
        m_wait_confirm_remote_node[node_id_a].l_r_result = local;
    }
}

void mcp::node_capability::set_remote_ack_hello(p2p::node_id node_id_a, bool remote)
{
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    if (m_wait_confirm_remote_node.count(node_id_a) > 0)
    {
        m_wait_confirm_remote_node[node_id_a].r_l_result = remote;
    }
}

bool mcp::node_capability::read_packet(std::shared_ptr<p2p::peer> peer_a, unsigned const & type, std::shared_ptr<dev::RLP> rlp)
{

    //LOG(m_log.trace) << "node id: " << peer_a->remote_node_id().to_string() << ", packet type: " << type << ", rlp: " << *rlp;
    //check remote node genesis is ok
    mcp::sub_packet_type pack_type = (mcp::sub_packet_type)type;
    if (pack_type != mcp::sub_packet_type::hello_info && pack_type != mcp::sub_packet_type::hello_info_request &&  pack_type != mcp::sub_packet_type::hello_info_ack)
    {
        if (is_remote_in_waitting_hello(peer_a->remote_node_id()))
        {
            return true;
        }
    }

    try
    {
        dev::RLP const & r(*rlp);
        switch (pack_type)
        {
        case mcp::sub_packet_type::joint:
        {
            bool error(r.itemCount() != 1);
            mcp::joint_message joint(error, r[0]);

            m_pcapability_metrics->joint++;
            if (error)
            {
                LOG(m_log.error) << "Invalid new block message rlp: " << r[0];
                peer_a->disconnect(p2p::disconnect_reason::bad_protocol);
                return true;
            }

            bool is_missing = false;
			bool need_add = true;
			mcp::block_hash block_hash(joint.block->hash());
			if (!joint.request_id.is_zero())
			{
				need_add = false;
				std::lock_guard<std::mutex> lock(m_requesting_lock);
				if (m_requesting.exist_erase(joint.request_id)) //ours request && broadcast not arrived
				{
					need_add = true;
					joint.level = mcp::joint_processor_level::request; //if block processor full also need add this block
				}
				joint.request_id.clear(); //broadcast do not need id
                is_missing = true;
			}
			else //broadcast try clear request hash
			{
				std::lock_guard<std::mutex> lock(m_requesting_lock);
				m_requesting.erase(block_hash);
			}

			if (need_add)
			{
				std::shared_ptr<mcp::block_processor_item> block_item_l(std::make_shared<mcp::block_processor_item>(joint, peer_a->remote_node_id()));
                if (is_missing)
                {
                    block_item_l->set_missing();
                }
                m_block_processor->add_to_mt_process(block_item_l);
			}

            //LOG(m_log.debug) << "Joint message, block hash: " << block_hash.to_string();
            mark_as_known_block(peer_a->remote_node_id(), block_hash);

            break;
        }
        case mcp::sub_packet_type::joint_request:
        {
            bool error(r.itemCount() != 1);
            mcp::joint_request_message request(error, r[0]);

            if (error)
            {
                LOG(m_log.error) << "Invalid joint request message rlp: " << r[0];
                peer_a->disconnect(p2p::disconnect_reason::bad_protocol);
                return true;
            }
            //LOG(m_log.trace) << "Recv joint request message, block hash " << request.block_hash.to_string();
			receive_joint_request_count++;
			m_async_task->sync_async([this, peer_a, request]() {
				m_sync->joint_request_handler(peer_a->remote_node_id(), request);
			});

            break;
        }
        case mcp::sub_packet_type::catchup_request:
        {
            bool error(r.itemCount() != 1);
            mcp::catchup_request_message request(error, r[0]);

            if (error)
            {
                LOG(m_log.error) << "Invalid catchup request message rlp: " << r[0];
                peer_a->disconnect(p2p::disconnect_reason::bad_protocol);
                return true;
            }

            //LOG(m_log.trace) << "Catchup request message, last stable mci: " << request.last_stable_mci
            //    << ", last known mci: " << request.last_known_mci
            //    << ", unstable_mc_joints_tail: " << request.unstable_mc_joints_tail.to_string()
            //    << ", first_catchup_chain_summary: " << request.first_catchup_chain_summary.to_string();

			receive_catchup_request_count++;
			m_async_task->sync_async([this, peer_a, request]() {
				m_sync->catchup_chain_request_handler(peer_a->remote_node_id(), request);
			});

            break;
        }
        case mcp::sub_packet_type::catchup_response:
        {
            bool error(r.itemCount() != 1);
            mcp::catchup_response_message response(error, r[0]);

            if (error)
            {
                LOG(m_log.error) << "Invalid catchup response message rlp: " << r[0];
                peer_a->disconnect(p2p::disconnect_reason::bad_protocol);
                return true;
            }

            //LOG(m_log.trace) << "Catchup response message, unstable_mc_joints size: " << response.unstable_mc_joints.size()
            //    << ", stable_last_summary_joints size: " << response.stable_last_summary_joints.size();

            if (m_sync->get_current_request_id() != response.request_id)
            {
                LOG(m_log.error) << "catchup_request request id error, response.request_id:" << response.request_id.to_string() << ",current_request_id:" << m_sync->get_current_request_id().to_string();
                return true;
            }

			if (m_sync->response_for_sync_request(peer_a->remote_node_id(), mcp::sub_packet_type::catchup_request))
			{
				receive_catchup_response_count++;
				m_async_task->sync_async([this, peer_a, response]() {
					m_sync->catchup_chain_response_handler(peer_a->remote_node_id(), response);
				});
			}
			
            break;
        }
        case mcp::sub_packet_type::hash_tree_request:
        {
            bool error(r.itemCount() != 1);
            mcp::hash_tree_request_message request(error, r[0]);

            if (error)
            {
                LOG(m_log.error) << "Invalid hash tree request message rlp: " << r[0];
                peer_a->disconnect(p2p::disconnect_reason::bad_protocol);
                return true;
            }

            //LOG(m_log.trace) << "recv hash tree request, from_summary " << request.from_summary.to_string()
            //    << ", to_summary: " << request.to_summary.to_string();

			receive_hash_tree_request_count++;
			m_async_task->sync_async([this, peer_a, request]() {
				m_sync->hash_tree_request_handler(peer_a->remote_node_id(), request);
			});

            break;
        }
        case mcp::sub_packet_type::hash_tree_response:
        {
            bool error(r.itemCount() != 1);
            mcp::hash_tree_response_message response(error, r[0]);

            if (error)
            {
                LOG(m_log.error) << "Invalid hash tree response message rlp: " << r[0];
                peer_a->disconnect(p2p::disconnect_reason::bad_protocol);
                return true;
            }

            //LOG(m_log.trace) << "recv hash tree response, arr_summary size: " << response.arr_summaries.size();
            if (m_sync->get_current_request_id() != response.request_id)
            {
                LOG(m_log.error) << "hash_tree_request:return timeout. response.request_id:" << response.request_id.to_string() << ",current_request_id:" << m_sync->get_current_request_id().to_string();
                return true;
            }

			receive_hash_tree_response_count++;
			if (m_sync->response_for_sync_request(peer_a->remote_node_id(), mcp::sub_packet_type::hash_tree_request))
			{
				m_async_task->sync_async([this, peer_a, response]() {
					m_sync->hash_tree_response_handler(peer_a->remote_node_id(), response);
				});
			}

            break;
        }
        case mcp::sub_packet_type::peer_info:
        {
			if (m_sync->is_syncing())
				return true;

            bool error(r.itemCount() != 1);
            mcp::peer_info_message pi(error, r[0]);

            if (error)
            {
                LOG(m_log.error) << "Invalid peer info message rlp: " << r[0];
                peer_a->disconnect(p2p::disconnect_reason::bad_protocol);
                return true;
            }

			receive_peer_info_count++;

			//dag
			for (auto it = pi.arr_tip_blocks.begin(); it != pi.arr_tip_blocks.end(); it++)
			{
				auto hash(std::make_shared<mcp::block_hash>(*it));

				m_async_task->sync_async([peer_a, hash, this]() {
					try
					{
						mcp::db::db_transaction transaction(m_store.create_transaction());
						if (!m_cache->block_exists(transaction, *hash))
						{
							m_block_arrival->remove(*hash);
							uint64_t _time = m_steady_clock.now_since_epoch();
							mcp::joint_request_item item(peer_a->remote_node_id(), hash, mcp::block_type::dag, mcp::requesting_block_cause::request_peer_info);
							item.from = mcp::joint_request_item_from::peerinfo;
							m_sync->request_new_missing_joints(item, _time);
						}
					}
					catch (const std::exception& e)
					{
						LOG(m_log.error) << "peer_info error:" << e.what();
						throw;
					}
				});
			}

			//light
			for (auto it = pi.arr_light_tip_blocks.begin(); it != pi.arr_light_tip_blocks.end(); it++)
			{
				auto hash(std::make_shared<mcp::block_hash>(it->second));
				if (m_block_processor->exist_in_clear_block_filter(*hash))
					continue;

				m_async_task->sync_async([peer_a, hash, this]() {
					try
					{
						mcp::db::db_transaction transaction(m_store.create_transaction());
						if (!m_cache->unlink_block_exists(transaction, *hash)
							&& !m_cache->block_exists(transaction, *hash))
						{
							m_block_arrival->remove(*hash);
							uint64_t _time = m_steady_clock.now_since_epoch();
							mcp::joint_request_item item(peer_a->remote_node_id(), hash, mcp::block_type::light, mcp::requesting_block_cause::request_peer_info);
							item.from = mcp::joint_request_item_from::peerinfo;
							m_sync->request_new_missing_joints(item, _time);
						}
					}
					catch (const std::exception& e)
					{
						LOG(m_log.error) << "peer_info error:" << e.what();
						throw;
					}
				});
			}

            break;
        }
        case mcp::sub_packet_type::peer_info_request:
        {
			receive_peer_info_request_count++;
            //LOG(m_log.trace) << "recv peer info request. ";
			m_async_task->sync_async([peer_a, this]() {
				try
				{
					mcp::p2p::node_id id(peer_a->remote_node_id());
					bool is_handle(false);
					{
						std::lock_guard<std::mutex> lock(m_peers_mutex);
						if (m_peers.count(id))
						{
							mcp::peer_info &pi(m_peers.at(id));
							if (auto p = pi.try_lock_peer())
							{
								std::chrono::steady_clock::time_point now(m_steady_clock.now());
								if (now - pi.last_hanlde_peer_info_request_time > std::chrono::milliseconds(COLLECT_PEER_INFO_INTERVAL / 2))
								{
									pi.last_hanlde_peer_info_request_time = now;
									is_handle = true;
								}
							}
						}
					}
					if (is_handle)
						m_sync->peer_info_request_handler(id);
				}
				catch (const std::exception& e)
				{
					LOG(m_log.error) << "peer_info_request error:" << e.what();
					throw;
				}
				
			});

            break;
        }
        case mcp::sub_packet_type::hello_info:
        {
            //LOG(m_log.trace) << "recv genesis info. ";
            bool error(r.itemCount() != 1);
            if (error)
            {
                LOG(m_log.error) << "Invalid genesis info message rlp: " << r[0];
                peer_a->disconnect(p2p::disconnect_reason::bad_protocol);
                return true;
            }

            mcp::block_hash genesis_hash_remote = (mcp::block_hash)r[0][0];

            if (check_remotenode_hello(genesis_hash_remote))
            {
                send_hello_info_ack(peer_a->remote_node_id());
                set_local_ack_hello(peer_a->remote_node_id(), true);
                if (is_local_remote_ack_ok_hello(peer_a->remote_node_id()))
                {
                    confirm_remote_connect(peer_a->remote_node_id());
                }
            }
            else
            {
                LOG(m_log.error) << "check_remotenode_genesis error: remote_node_id = " << peer_a->remote_node_id().to_string();
                peer_a->disconnect(p2p::disconnect_reason::bad_protocol);
            }

            break;
        }
        case mcp::sub_packet_type::hello_info_request:
        {
            //LOG(m_log.trace) << "recv genesis info request.";
            send_hello_info(peer_a->remote_node_id());
            break;
        }
        case mcp::sub_packet_type::hello_info_ack:
        {
            //LOG(m_log.trace) << "recv genesis info confirm.";
            set_remote_ack_hello(peer_a->remote_node_id(), true);
            if (is_local_remote_ack_ok_hello(peer_a->remote_node_id()))
            {
                confirm_remote_connect(peer_a->remote_node_id());
            }
            break;
        }
        default:
            return false;
        }
    }
    catch (std::exception const & e)
    {
        LOG(m_log.trace) << "Peer error, node id: " << peer_a->remote_node_id().to_string()
            << ", packet type: " << type << ", rlp: " << *rlp << ", message: " << e.what();
        throw;
    }

    return true;
}


void mcp::node_capability::broadcast_block(mcp::joint_message const & message)
{
	try
	{
		mcp::block_hash block_hash(message.block->hash());
		std::lock_guard<std::mutex> lock(m_peers_mutex);
		for (auto it = m_peers.begin(); it != m_peers.end();)
		{
			mcp::peer_info &pi(it->second);
			if (auto p = pi.try_lock_peer())
			{
				it++;
				if (pi.is_known_block(block_hash))
					continue;

				m_pcapability_metrics->broadcast_joint++;
				m_node_id_cap_metrics[p->remote_node_id()]->broadcast_joint++;

				dev::RLPStream s;
				p->prep(s, pi.offset + (unsigned)mcp::sub_packet_type::joint, 1);
				message.stream_RLP(s);
				p->send(s);
			}
			else
				it = m_peers.erase(it);
		}
	}
	catch (const std::exception& e)
	{
		LOG(m_log.error) << "broadcast_block error, error: " << e.what();
		throw;
	}
}

void mcp::node_capability::mark_as_known_block(p2p::node_id node_id_a, mcp::block_hash block_hash_a)
{
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    if (m_peers.count(node_id_a))
        m_peers.at(node_id_a).mark_as_known_block(block_hash_a);
}


uint64_t mcp::node_capability::num_peers()
{
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    for (auto it = m_peers.begin(); it != m_peers.end();)
    {
        mcp::peer_info &pi(it->second);
        if (auto p = pi.try_lock_peer())
        {
            it++;
        }
        else
            it = m_peers.erase(it);
    }

    return m_peers.size();
}

void mcp::node_capability::send_hello_info(p2p::node_id const &id)
{
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    if (m_peers.count(id))
    {
        mcp::peer_info &pi(m_peers.at(id));
        if (auto p = pi.try_lock_peer())
        {
            m_pcapability_metrics->hello_info++;
            m_node_id_cap_metrics[id]->hello_info++;

            dev::RLPStream s;
            p->prep(s, pi.offset + (unsigned)mcp::sub_packet_type::hello_info, 1);
            s.appendList(2);
            s << m_genesis;
            s << desc.version;
            p->send(s);
        }
    }

}

void mcp::node_capability::send_hello_info_request(p2p::node_id const &id)
{
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    if (m_peers.count(id))
    {
        mcp::peer_info &pi(m_peers.at(id));
        if (auto p = pi.try_lock_peer())
        {
            m_pcapability_metrics->hello_info_request++;
            m_node_id_cap_metrics[id]->hello_info_request++;

            dev::RLPStream s;
            p->prep(s, pi.offset + (unsigned)mcp::sub_packet_type::hello_info_request, 1);
            s.appendList(0);
            p->send(s);
        }
    }
}

void mcp::node_capability::send_hello_info_ack(p2p::node_id const &id)
{
    std::lock_guard<std::mutex> lock(m_peers_mutex);
    if (m_peers.count(id))
    {
        mcp::peer_info &pi(m_peers.at(id));
        if (auto p = pi.try_lock_peer())
        {
            m_pcapability_metrics->hello_info_ack++;
            m_node_id_cap_metrics[id]->hello_info_ack++;

            dev::RLPStream s;
            p->prep(s, pi.offset + (unsigned)mcp::sub_packet_type::hello_info_ack, 1);
            s.appendList(0);
            p->send(s);
        }
    }
}

mcp::sync_request_hash  mcp::node_capability::gen_sync_request_hash(p2p::node_id const & id, uint64_t random, mcp::sub_packet_type & request_type_a)
{
	mcp::sync_request_hash result;
	blake2b_state hash_l;
	auto status(blake2b_init(&hash_l, sizeof(result.bytes)));

	assert_x(status == 0);
	size_t random_l = mcp::random_pool.GenerateWord32();
	blake2b_update(&hash_l, id.bytes.data(), sizeof(id.bytes));
	blake2b_update(&hash_l, &random, sizeof(random));
	blake2b_update(&hash_l, &request_type_a, sizeof(request_type_a));
	blake2b_update(&hash_l, &random_l, sizeof(random_l));


	status = blake2b_final(&hash_l, result.bytes.data(), sizeof(result.bytes));
	assert_x(status == 0);

	return result;
}


//return: true:successed, false: requesting in the way
bool mcp::requesting_mageger::add(mcp::requesting_item& item_a, bool const& count_a)
{
	bool ret = true;
	auto it(m_request_info.get<1>().find(item_a.m_request_hash));
	if (it != m_request_info.get<1>().end())
	{
		if (item_a.m_time > it->m_time + mcp::requesting_mageger::STALLED_TIMEOUT) //allow request again
		{
			item_a.m_request_id = it->m_request_id; //used same request id as before
			//upgrade request time
			mcp::requesting_item item(*it);
			item.m_time = item_a.m_time;
			if (count_a)
				item.m_request_count++;
			m_request_info.get<1>().replace(it, item);
		}
		else
		{
			ret = false;
		}
	}
	else
	{
		item_a.m_request_id = m_capability.gen_sync_request_hash(item_a.m_node_id, m_random_uint, item_a.m_type);
		m_random_uint++;

		m_request_info.insert(item_a);
	}
	return ret;
}

bool mcp::requesting_mageger::exist_erase(mcp::sync_request_hash const & request_id_a)
{
	auto it(m_request_info.get<0>().find(request_id_a));
	if (it != m_request_info.get<0>().end())
	{
		m_request_info.get<0>().erase(it);
		return true;
	}
	arrival_filter_count++;
	return false;
}

void mcp::requesting_mageger::erase(mcp::block_hash const& hash_a)
{
	auto it(m_request_info.get<1>().find(hash_a));
	if (it != m_request_info.get<1>().end())
	{
		m_request_info.get<1>().erase(it);
	}
}

std::list<mcp::requesting_item> mcp::requesting_mageger::clear_by_time(uint64_t const& time_a)
{
	std::list<mcp::requesting_item> result;
	for (auto it = m_request_info.get<2>().begin(); it != m_request_info.get<2>().end();)
	{
		if (time_a > it->m_time + STALLED_TIMEOUT)
		{
			if (it->m_request_count > RETYR_TIMES)
			{
				it = m_request_info.get<2>().erase(it);
			}
			else
			{
				result.push_back(*it);
				it++;
			}
		}
		else
			break;
	}
	return result;
}

std::string mcp::requesting_mageger::get_info()
{
	std::string ret = " ,arrival_filter_count:" + std::to_string(arrival_filter_count);
	return ret;
}
