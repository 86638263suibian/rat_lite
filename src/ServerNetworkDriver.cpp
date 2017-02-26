#include "ServerNetworkDriver.h"
#include "Shapes.h"
#include "IServerDriver.h"
#include "internal/WebSocket.h"
#include "Configs.h"
#include "turbojpeg.h"
#include "Input.h"
#include "Logging.h"
#include "ScreenCapture.h"
#include "uWS.h"
#include "SCCommon.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <assert.h>
#include <array>
#include <string>

namespace SL {
	namespace RAT {

		class ServerNetworkDriverImpl {

		public:
			IServerDriver* _IServerDriver;
			std::shared_ptr<Server_Config> Config_;
			std::vector<uWS::Hub*> threads_;
			uWS::Hub h;
			std::thread Runner;
			std::atomic_int ClientCount;

			ServerNetworkDriverImpl(IServerDriver * r, std::shared_ptr<Server_Config> config) :
				_IServerDriver(r), Config_(config) {
				ClientCount = 0;

				threads_.resize(config->MaxWebSocketThreads);

				h.onConnection([&](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
					static int counter = 0;
				
					ws.setUserData(new std::mutex);

					if (ClientCount + 1 > Config_->MaxNumConnections) {
						char msg[] = "Closing due to max number of connections!";
						ws.close(1000, msg, sizeof(msg));
					}
					else {

						int t = counter++ % Config_->MaxWebSocketThreads;
						SL_RAT_LOG(Logging_Levels::INFO_log_level, "Transfering connection to thread " << t);

						ws.transfer(&threads_[t]->getDefaultGroup<uWS::SERVER>());
						_IServerDriver->onConnection(std::make_shared<WebSocket<uWS::WebSocket<uWS::SERVER>>>(ws));
						ClientCount += 1;
					}
				});

				for (size_t i = 0; i < threads_.size(); i++) {
					new std::thread([&, i] {
						// register our events

						threads_[i] = new uWS::Hub();
						threads_[i]->getDefaultGroup<uWS::SERVER>().setUserData(new std::mutex);

						threads_[i]->onDisconnection([&, i](uWS::WebSocket<uWS::SERVER> ws, int code, char *message, size_t length) {
							SL_RAT_LOG(Logging_Levels::INFO_log_level, "onDisconnection on thread " << i);
							WebSocket<uWS::WebSocket<uWS::SERVER>> sock(ws, (std::mutex*)threads_[i]->getDefaultGroup<uWS::SERVER>().getUserData());
							ClientCount -= 1;
							_IServerDriver->onDisconnection(sock, code, message, length);
						
						});
						threads_[i]->onMessage([&, i](uWS::WebSocket<uWS::SERVER> ws, char *message, size_t length, uWS::OpCode code) {
							SL_RAT_LOG(Logging_Levels::INFO_log_level, "onMessage on thread " << i);
							WebSocket<uWS::WebSocket<uWS::SERVER>> sock(ws, (std::mutex*)threads_[i]->getDefaultGroup<uWS::SERVER>().getUserData());
							auto pactype = PACKET_TYPES::INVALID;
							assert(length >= sizeof(pactype));

							pactype = *reinterpret_cast<const PACKET_TYPES*>(message);
							length -= sizeof(pactype);
							message += sizeof(pactype);

							switch (pactype) {
							case PACKET_TYPES::MOUSEEVENT:
								assert(length == sizeof(MouseEvent));
								_IServerDriver->onReceive_Mouse(reinterpret_cast<const MouseEvent*>(message));
								break;
							case PACKET_TYPES::KEYEVENT:
								assert(length == sizeof(KeyEvent));
								_IServerDriver->onReceive_Key(reinterpret_cast<const KeyEvent*>(message));
								break;
							case PACKET_TYPES::CLIPBOARDTEXTEVENT:
								_IServerDriver->onReceive_ClipboardText(message, length);
								break;
							default:
								_IServerDriver->onMessage(sock, message - sizeof(pactype), length + sizeof(pactype));
								break;
							}

						});

						threads_[i]->getDefaultGroup<uWS::SERVER>().addAsync();
						threads_[i]->run();
						delete (std::mutex*)threads_[i]->getDefaultGroup<uWS::SERVER>().getUserData();
					});
				}

				//uS::TLS::Context c = uS::TLS::createContext(config->PathTo_Public_Certficate, config->PathTo_Private_Key, config->PasswordToPrivateKey);

				h.listen(Config_->WebSocketTLSLPort, nullptr, 0, nullptr);

			}
			virtual ~ServerNetworkDriverImpl() {

				if (Runner.joinable()) {
					Runner.join();
				}
			}
			void Send(IWebSocket* socket, char* data, size_t len) {

				if (socket) {
					socket->send(data, len);
				}
				else {
					for (auto& a : threads_) {
						std::lock_guard<std::mutex> lock(*(std::mutex*)a->getDefaultGroup<uWS::SERVER>().getUserData());
						a->getDefaultGroup<uWS::SERVER>().broadcast(data, len, uWS::OpCode::BINARY);
					}
				}

			}
			void Run() {
				Runner = std::thread([&]() { h.run(); });
			}
			void SendScreen(IWebSocket* socket, const Screen_Capture::Image & img, const SL::Screen_Capture::Monitor& monitor,  PACKET_TYPES p) {

				if (ClientCount <= 0) return;
				Rect r(Point(0, 0), Height(img), Width(img));

				auto set = Config_->SendGrayScaleImages ? TJSAMP_GRAY : TJSAMP_420;
				auto maxsize = tjBufSize(Screen_Capture::Width(img), Screen_Capture::Height(img), set)  + sizeof(r) + sizeof(p) + sizeof(monitor.Id);

				thread_local auto jpegCompressor = tjInitCompress();
				auto  buffer = std::make_unique<char[]>(maxsize);
			
				auto dst = (unsigned char*)buffer.get();
				memcpy(dst, &p, sizeof(p));
				dst += sizeof(p);
				memcpy(dst, &monitor.Id, sizeof(monitor.Id));
				dst += sizeof(monitor.Id);
				memcpy(dst, &r, sizeof(r));
				dst += sizeof(r);

				auto srcbuffer = std::make_unique<char[]>(RowStride(img)*Height(img));
				Screen_Capture::Extract(img, srcbuffer.get(), RowStride(img)*Height(img));
				auto srcbuf = (const unsigned char*)srcbuffer.get();

#if __ANDROID__
				auto colorencoding = TJPF_RGBX;
#else 
				auto colorencoding = TJPF_BGRX;
#endif
				auto outjpegsize = maxsize;
				if (tjCompress2(jpegCompressor, srcbuf, r.Width, 0, r.Height, colorencoding, &dst, &outjpegsize, set, Config_->ImageCompressionSetting, TJFLAG_FASTDCT | TJFLAG_NOREALLOC) == -1) {
					SL_RAT_LOG(Logging_Levels::ERROR_log_level, tjGetErrorStr());
				}
				//	std::cout << "Sending " << r << std::endl;
				auto finalsize = sizeof(p) + sizeof(r) + sizeof(monitor.Id) + outjpegsize;//adjust the correct size
			
				Send(socket, buffer.get(), finalsize);
			}
			void SendMouse(IWebSocket* socket, const Screen_Capture::Image & img) {

				if (ClientCount <= 0) return;
				Rect r(Point(0, 0), Height(img), Width(img));

				auto p = static_cast<unsigned int>(PACKET_TYPES::MOUSEIMAGE);
				auto finalsize = (Screen_Capture::RowStride(img) * Screen_Capture::Height(img)) + sizeof(p) + sizeof(r);
				auto buffer = std::make_unique<char[]>(finalsize);

				auto dst = buffer.get();
				memcpy(dst, &p, sizeof(p));
				dst += sizeof(p);
				memcpy(dst, &r, sizeof(r));
				dst += sizeof(r);
				Screen_Capture::Extract(img, dst, Screen_Capture::RowStride(img) * Screen_Capture::Height(img));

				Send(socket, buffer.get(), finalsize);

			}
			void SendMonitorInfo(IWebSocket * socket, const std::vector<std::shared_ptr<Screen_Capture::Monitor>>& monitors) {
				
				auto p = static_cast<unsigned int>(PACKET_TYPES::MONITORINFO);
				const auto size = (monitors.size() * sizeof(Screen_Capture::Monitor)) + sizeof(p);

				auto buffer = std::make_unique<char[]>(size);
				auto buf = buffer.get();
				memcpy(buf, &p, sizeof(p));
				buf += sizeof(p);
				for (auto& a : monitors) {
					memcpy(buf, a.get(), sizeof(Screen_Capture::Monitor));
					buf += sizeof(Screen_Capture::Monitor);
				}
				Send(socket, buffer.get(), size);

			}
			void SendMouse(IWebSocket* socket, const Point& pos)
			{
				if (ClientCount <= 0) return;
				auto p = static_cast<unsigned int>(PACKET_TYPES::MOUSEPOS);
				const auto size = sizeof(pos) + sizeof(p);
				char buffer[size];

				memcpy(buffer, &p, sizeof(p));
				memcpy(buffer + sizeof(p), &pos, sizeof(pos));

				Send(socket, buffer, size);
			}

			void SendClipboardText(IWebSocket* socket, const char* data, unsigned int len) {
				if (ClientCount <= 0) return;
				auto p = static_cast<unsigned int>(PACKET_TYPES::CLIPBOARDTEXTEVENT);
				auto size = len + sizeof(p);

				auto buffer = std::make_unique<char[]>(size);
				memcpy(buffer.get(), &p, sizeof(p));
				memcpy(buffer.get() + sizeof(p), data, len);

				Send(socket, buffer.get(), size);

			}
		};
		ServerNetworkDriver::ServerNetworkDriver()
		{

		}
		ServerNetworkDriver::~ServerNetworkDriver()
		{

		}
		void ServerNetworkDriver::Start(IServerDriver * r, std::shared_ptr<Server_Config> config) {
			_ServerNetworkDriverImpl = std::make_unique<ServerNetworkDriverImpl>(r, config);
			_ServerNetworkDriverImpl->Run();
		}
		void ServerNetworkDriver::Stop() {
			_ServerNetworkDriverImpl.reset();
		}

		void ServerNetworkDriver::SendFrameChange(IWebSocket* socket, const Screen_Capture::Image & img, const SL::Screen_Capture::Monitor& monitor)
		{
			_ServerNetworkDriverImpl->SendScreen(socket, img, monitor, PACKET_TYPES::SCREENIMAGEDIF);
		}
		void ServerNetworkDriver::SendFrame(IWebSocket* socket, const Screen_Capture::Image & img, const SL::Screen_Capture::Monitor& monitor)
		{
			_ServerNetworkDriverImpl->SendScreen(socket, img, monitor, PACKET_TYPES::SCREENIMAGE);
		}
		void ServerNetworkDriver::SendMonitorInfo(IWebSocket * socket, const std::vector<std::shared_ptr<Screen_Capture::Monitor>>& monitors)
		{
			_ServerNetworkDriverImpl->SendMonitorInfo(socket, monitors);
		}
		void ServerNetworkDriver::SendMouse(IWebSocket* socket, const Screen_Capture::Image & img)
		{
			_ServerNetworkDriverImpl->SendMouse(socket, img);
		}
		void ServerNetworkDriver::SendMouse(IWebSocket* socket, const Point & pos)
		{
			_ServerNetworkDriverImpl->SendMouse(socket, pos);
		}

		void ServerNetworkDriver::SendClipboardText(IWebSocket* socket, const char* data, unsigned int len) {
			_ServerNetworkDriverImpl->SendClipboardText(socket, data, len);
		}


	}
}
