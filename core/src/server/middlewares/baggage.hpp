#pragma once

#include <userver/server/middlewares/http_middleware_base.hpp>

USERVER_NAMESPACE_BEGIN

namespace server::middlewares {

class Baggage final : public HttpMiddlewareBase {
 public:
  static constexpr std::string_view kName{"userver-baggage-middleware"};

  Baggage(const handlers::HttpHandlerBase&, const components::ComponentConfig&);

 private:
  void HandleRequest(http::HttpRequest& request,
                     request::RequestContext& context) const override;
};

using BaggageFactory = SimpleHttpMiddlewareFactory<Baggage>;

}  // namespace server::middlewares

USERVER_NAMESPACE_END