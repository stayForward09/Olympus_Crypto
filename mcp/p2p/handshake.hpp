#pragma once

#include <mcp/p2p/common.hpp>
#include <mcp/p2p/capability.hpp>
#include <mcp/p2p/host.hpp>
#include <mcp/common/common.hpp>
#include <mcp/common/log.hpp>
#include <mcp/core/config.hpp>

namespace mcp
{
	namespace p2p
	{
		class hankshake_msg
		{
		public:
			hankshake_msg(node_id const & node_id_a, uint16_t const & version_a, mcp::mcp_networks const & network_a, std::list<capability_desc> const & cap_descs_a);
			hankshake_msg(dev::RLP const & r);
			void stream_RLP(dev::RLPStream & s);

			node_id id;
			uint16_t version;
			mcp::mcp_networks network;
			std::list<capability_desc> cap_descs;
		};

		class host;
		class RLPXFrameCoder;
		class hankshake : public std::enable_shared_from_this<hankshake>
		{
			friend class RLPXFrameCoder;
		public:
			hankshake(std::shared_ptr<host> const& _host, std::shared_ptr<bi::tcp::socket> const& _socket) :
				hankshake(_host, _socket, {})
			{
			}

			/// originated connection.
			hankshake(std::shared_ptr<host> const& _host, std::shared_ptr<bi::tcp::socket> const& _socket, node_id _remote) :
				m_host(_host),
				m_remote(_remote),
				m_originated(_remote),
				m_socket(_socket),
				m_idleTimer(m_socket->get_io_service()),
				m_failureReason{ HandshakeFailureReason::NoFailure }
			{
				crypto::Nonce::get().ref().copyTo(m_nonce.ref());
			}

			virtual ~hankshake() = default;

			/// Start handshake.
			void start() { transition(); }

			/// Aborts the handshake.
			void cancel();

			node_id remote() const { return m_remote; }

		private:
			enum State
			{
				Error = -1,
				ExchgPublic,
				AckExchgPublic,
				New,
				AckAuth,
				AckAuthEIP8,
				WriteHello,
				ReadHello,
				StartSession
			};

			///
			void ComposeOutPacket(dev::bytes const& data);

			/// send to socket
			void send(dev::bytes const& data);

			/// read from socket
			void read();

			///get packet size
			uint32_t packet_size();

			///process read packet
			void do_process();

			///write p2p info message to socket, version, public key, transaction to readPublic.
			void writeInfo();

			///reads info message from socket and transitions to writePublic.
			void readInfo();

			/// Write Auth message to socket and transitions to AckAuth.
			void writeAuth();

			/// Reads Auth message from socket and transitions to AckAuth.
			void readAuth();

			/// Write Ack message to socket and transitions to WriteHello.
			void writeAck();

			/// Reads Auth message from socket and transitions to WriteHello.
			void readAck();

			/// Derives ephemeral secret from signature and sets members after Auth has been decrypted.
			void setAuthValues(dev::Signature const& sig, Public const& remotePubk, h256 const& remoteNonce);

			///handshake error 
			void error();

			/// Performs transition for m_nextState.
			virtual void transition(boost::system::error_code _ech = boost::system::error_code());

			/// Timeout for remote to respond to transition events. Enforced by m_idleTimer and refreshed by transition().
			boost::posix_time::milliseconds const c_timeout = boost::posix_time::milliseconds(1800);

			State m_curState = ExchgPublic;
			State m_nextState = ExchgPublic;	///Current or expected state of transition.
			bool m_cancel = false;	///true if error occured

			std::shared_ptr<host> m_host;
			node_id m_remote;	///Public address of remote host.
			bool m_originated = false;	///True if connection is outbound.

			bytes m_authCipher;				///< Ciphertext of egress or ingress Auth message.
			bytes m_ackCipher;				///< Ciphertext of egress or ingress Ack message.
			dev::bytes m_handshakeOutBuffer;///< Frame buffer for egress Hello packet.
			dev::bytes m_handshakeInBuffer;	///< Frame buffer for ingress Hello packet.

			KeyPair m_ecdheLocal = KeyPair::create();  ///< Ephemeral ECDH secret and agreement.
			h256 m_nonce;					///< Nonce generated by this host for handshake.

			Public m_ecdheRemote;			///< Remote ephemeral public key.
			h256 m_remoteNonce;				///< nonce generated by remote host for handshake.
			uint64_t m_remoteVersion = 0;

			std::unique_ptr<mcp::p2p::RLPXFrameCoder> m_io;

			std::shared_ptr<bi::tcp::socket> m_socket;

			boost::asio::deadline_timer m_idleTimer;	///< Timer which enforces c_timeout.

			HandshakeFailureReason m_failureReason;

            mcp::log m_log = { mcp::log("p2p") };
		};
	}
}