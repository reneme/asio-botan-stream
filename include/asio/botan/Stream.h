#pragma once
#include <type_traits>
#include <memory>
#include <thread>
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <botan/tls_channel.h>
#include <botan/tls_client.h>
#include <botan/tls_server.h>
#include <botan/auto_rng.h>

#include "detail/StreamCore.h"
#include "detail/AsyncReadOperation.h"
#include "detail/AsyncWriteOperation.h"
#include "detail/AsyncHandshakeOperation.h"

namespace asio
{
	namespace botan
	{

		template<class Channel>
		class StreamBase
		{ };

		template<>
		class StreamBase<Botan::TLS::Client>
		{
		public:
			StreamBase(Botan::TLS::Session_Manager& sessionManager, Botan::Credentials_Manager& credentialsManager, Botan::TLS::Policy& policy, const Botan::TLS::Server_Information& serverInfo = Botan::TLS::Server_Information{})
				: channel_(core_, sessionManager, credentialsManager, policy, rng_, serverInfo)
			{
			}

			StreamBase(StreamBase&) = delete;
			StreamBase& operator=(StreamBase&) = delete;
		protected:
			detail::StreamCore core_;
			Botan::AutoSeeded_RNG rng_;
			Botan::TLS::Client channel_;
		};


		template<>
		class StreamBase<Botan::TLS::Server>
		{
		public:
			StreamBase(Botan::TLS::Session_Manager& sessionManager, Botan::Credentials_Manager& credentialsManager, Botan::TLS::Policy& policy)
				: channel_(core_, sessionManager, credentialsManager, policy, rng_)
			{
			}

			StreamBase(StreamBase&) = delete;
			StreamBase& operator=(StreamBase&) = delete;
		protected:
			detail::StreamCore core_;
			Botan::AutoSeeded_RNG rng_;
			Botan::TLS::Server channel_;
		};

		template<class StreamLayer, class Channel>
		class Stream : public StreamBase<Channel>
		{
		public:
			using next_layer_type = std::remove_reference_t<StreamLayer>;

			using lowest_layer_type = typename next_layer_type::lowest_layer_type;

			using executor_type = typename lowest_layer_type::executor_type;

			template<typename ...Args>
			Stream(StreamLayer nextLayer, Args&& ... args)
				: StreamBase(std::forward<Args>(args)...),
				nextLayer_(std::move(nextLayer))
			{
			}


			executor_type get_executor() noexcept
			{
				return nextLayer_.get_executor();
			}

			const next_layer_type& next_layer() const
			{
				return nextLayer_;
			}
			next_layer_type& next_layer()
			{
				return nextLayer_;
			}

			lowest_layer_type& lowest_layer()
			{
				return nextLayer_.lowest_layer();
			}

			void handshake(boost::system::error_code& ec)
			{
				while (!channel_.is_active())
				{
					writePendingTlsData(ec);
					auto read_buffer = boost::asio::buffer(core_.input_buffer_, nextLayer_.read_some(core_.input_buffer_));
					try
					{
						channel_.received_data(static_cast<const uint8_t*>(read_buffer.data()), read_buffer.size());
					}
					catch (std::exception& e)
					{
						ec = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
						return;
					}
				}
			}

			template <typename HandshakeHandler>
			BOOST_ASIO_INITFN_RESULT_TYPE(HandshakeHandler,
				void(boost::system::error_code))
				async_handshake(HandshakeHandler&& handler)
			{
				// If you get an error on the following line it means that your handler does
				// not meet the documented type requirements for a HandshakeHandler.
				BOOST_ASIO_HANDSHAKE_HANDLER_CHECK(HandshakeHandler, handler) type_check;

				boost::asio::async_completion<HandshakeHandler,
					void(boost::system::error_code)> init(handler);

				auto op = create_async_handshake_op(init.completion_handler);
				op(boost::system::error_code{}, 0, 1);

				return init.result.get();
			}

			void shutdown(boost::system::error_code& ec)
			{
				channel_.close();
				writePendingTlsData(ec);
			}


			template <typename MutableBufferSequence>
			std::size_t read_some(const MutableBufferSequence& buffers)
			{
				boost::system::error_code ec;
				std::size_t n = read_some(buffers, ec);
				boost::asio::detail::throw_error(ec, "read_some");
				return n;
			}

			template <typename MutableBufferSequence>
			std::size_t read_some(const MutableBufferSequence& buffers,
				boost::system::error_code& ec)
			{
				while (core_.received_data_.size() == 0)
				{
					auto read_buffer = boost::asio::buffer(core_.input_buffer_, nextLayer_.read_some(core_.input_buffer_, ec));
					if (ec)
						return 0;
					channel_.received_data(static_cast<const uint8_t*>(read_buffer.data()), read_buffer.size());
				}

				auto copied = boost::asio::buffer_copy(buffers, core_.received_data_.data());
				core_.received_data_.consume(copied);
				ec = boost::system::error_code();
				return copied;
			}



			template <typename ConstBufferSequence>
			std::size_t write_some(const ConstBufferSequence& buffers)
			{
				boost::system::error_code ec;
				std::size_t n = write_some(buffers, ec);
				boost::asio::detail::throw_error(ec, "write_some");
				return n;
			}

			template <typename ConstBufferSequence>
			std::size_t write_some(const ConstBufferSequence& buffers,
				boost::system::error_code& ec)
			{
				std::unique_lock<std::recursive_mutex> lock(core_.sendMutex_);
				boost::asio::const_buffer buffer =
					boost::asio::detail::buffer_sequence_adapter<boost::asio::const_buffer,
					ConstBufferSequence>::first(buffers);

				channel_.send(static_cast<const uint8_t*>(buffer.data()), buffer.size());

				writePendingTlsData(ec);
				return buffer.size();
			}

			template <typename ConstBufferSequence, typename WriteHandler>
			BOOST_ASIO_INITFN_RESULT_TYPE(WriteHandler,
				void(boost::system::error_code, std::size_t))
				async_write_some(const ConstBufferSequence& buffers, WriteHandler&& handler)
			{
				// If you get an error on the following line it means that your handler does
				// not meet the documented type requirements for a WriteHandler.
				BOOST_ASIO_WRITE_HANDLER_CHECK(WriteHandler, handler) type_check;

				std::unique_lock<std::recursive_mutex> lock(core_.sendMutex_);

				boost::asio::const_buffer buffer =
					boost::asio::detail::buffer_sequence_adapter<boost::asio::const_buffer,
					ConstBufferSequence>::first(buffers);

				channel_.send(static_cast<const uint8_t*>(buffer.data()), buffer.size());

				boost::asio::async_completion<WriteHandler,
					void(boost::system::error_code, std::size_t)> init(handler);
				auto op = create_async_write_op(init.completion_handler, buffer.size());

				boost::asio::async_write(nextLayer_, core_.send_data_.data(), std::move(op));
				return init.result.get();
			}

			template <typename MutableBufferSequence, typename ReadHandler>
			BOOST_ASIO_INITFN_RESULT_TYPE(ReadHandler,
				void(boost::system::error_code, std::size_t))
				async_read_some(const MutableBufferSequence& buffers,
					BOOST_ASIO_MOVE_ARG(ReadHandler) handler)
			{
				// If you get an error on the following line it means that your handler does
				// not meet the documented type requirements for a ReadHandler.
				BOOST_ASIO_READ_HANDLER_CHECK(ReadHandler, handler) type_check;

				boost::asio::async_completion<ReadHandler,
					void(boost::system::error_code, std::size_t)> init(handler);

				auto op = create_async_read_op(init.completion_handler, buffers);
				op(boost::system::error_code{}, 0, 1);
				return init.result.get();
			}

		protected:

			size_t writePendingTlsData(boost::system::error_code& ec)
			{
				std::unique_lock<std::recursive_mutex> lock(core_.sendMutex_);
				auto writtenBytes = boost::asio::write(nextLayer_, core_.send_data_.data(), ec);
				core_.send_data_.consume(writtenBytes);
				return writtenBytes;
			}

			template<typename Handler>
			detail::AsyncHandshakeOperation<StreamLayer, Handler> create_async_handshake_op(Handler& handler)
			{
				return detail::AsyncHandshakeOperation<StreamLayer, Handler>(channel_, core_, nextLayer_, handler);
			}

			template<typename Handler, typename MutableBufferSequence>
			detail::AsyncReadOperation<StreamLayer, Handler, MutableBufferSequence> create_async_read_op(Handler& handler, const MutableBufferSequence& buffers)
			{
				return detail::AsyncReadOperation<StreamLayer, Handler, MutableBufferSequence>(channel_, core_, nextLayer_, handler, buffers);
			}

			template<typename Handler>
			detail::AsyncWriteOperation<Handler> create_async_write_op(Handler& handler, std::size_t plainBytesTransferred)
			{
				return detail::AsyncWriteOperation<Handler>(core_, handler, plainBytesTransferred);
			}

			StreamLayer nextLayer_;
		};

		template<class StreamLayer>
		using ClientStream = Stream<StreamLayer, Botan::TLS::Client>;

		template<class StreamLayer>
		using ServerStream = Stream<StreamLayer, Botan::TLS::Server>;
	}
}
