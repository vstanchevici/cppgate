#include <catch2/catch_test_macros.hpp>

//https://gist.github.com/chezou/1395527
#include <Core/Router.h>

uint32_t router_func(uint32_t number)
{
    gtvr::router::Router router;

    /*
    router.add_route({ boost::beast::http::verb::get, boost::beast::http::verb::post }, R"(/users/{id})", [](gtvr::Context& ctx) {
        if (req.method == boost::beast::http::verb::get) {
            std::cout << "[GET] Fetch user: " << req.params.at("id") << std::endl;
        }
        else if (req.method == boost::beast::http::verb::post) {
            std::cout << "[POST] Update user: " << req.params.at("id") << std::endl;
        }

        return true;
    });

    router.add_route({ boost::beast::http::verb::delete_ }, R"(/users/{id})", [](gtvr::Context& ctx) {
        std::cout << "[DELETE] Remove user: " << req.params.at("id") << std::endl;
        return true;
    });

    router.add_route({ boost::beast::http::verb::get }, R"(/)", [](gtvr::Context& ctx) {
        std::cout << "[GET] Root path: " << req.path << std::endl;
        return true;
    });

    router.add_route({ boost::beast::http::verb::get }, R"(/api/v1/users/{id}.{format})", [](gtvr::Context& ctx) {
        std::cout << "[GET] Fetch user: " << req.params.at("id") << " with format " << req.params.at("format") << std::endl;
        return true;
    });
   

    gtvr::Handler auth = [](gtvr::Context& ctx)
    {
        if (req.params["token"] != "secret")
        {
            res.send(403, "Forbidden");
            return false;
        }

        return true;
    };

    gtvr::Handler logger = [](gtvr::Context& ctx)
    {
        std::cout << ">> " << req.method << " " << req.path << "\n";
        return true;
    };

    
    // Group example:
    router.group("/api/v1", { logger }, [auth](gtvr::Router& r)
    {
        r.add_route({ boost::beast::http::verb::get }, "/users/{id}", [](gtvr::Context& ctx) {
            res.status = 200;
            res.body = "User info: " + req.params.at("id");
            return true;
        });

        // Nested group for /admin
        r.group("/admin", { auth }, [](gtvr::Router& r) {
            r.add_route({ boost::beast::http::verb::get }, "/dashboard", [](gtvr::Context& ctx) {
                // Handler for /admin/dashboard
                res.status = 200;//http::status::ok;
                res.body = "Admin Dashboard";
                //res.prepare_payload();
                //respond(std::move(res));
                return true;
            });
        });
    });
    

    // Simulate HTTP requests
    gtvr::Request req1{ boost::beast::http::verb::get, "/api/v1/users/alice.xml" };
    gtvr::Response res1;
    router.route(req1, res1);
    std::cout << res1.status << " - " << res1.body << "\n";

    gtvr::Request req2{ boost::beast::http::verb::get, "/users/42" };
    gtvr::Response res2;
    router.route(req2, res2);
    std::cout << res2.status << " - " << res2.body << "\n";

    gtvr::Request req3{ boost::beast::http::verb::post, "/users/42" };
    gtvr::Response res3;
    router.route(req3, res3);
    std::cout << res3.status << " - " << res3.body << "\n";

    gtvr::Request req4{ boost::beast::http::verb::delete_, "/users/42" };
    gtvr::Response res4;
    router.route(req4, res4);
    std::cout << res4.status << " - " << res4.body << "\n";

    gtvr::Request req5{ boost::beast::http::verb::get, "/" };
    gtvr::Response res5;
    router.route(req5, res5);
    std::cout << res5.status << " - " << res5.body << "\n";

    gtvr::Request req6{ boost::beast::http::verb::put, "/users/42" }; // Should 405
    gtvr::Response res6;
    router.route(req6, res6);
    std::cout << res6.status << " - " << res6.body << "\n";

    gtvr::Request req7{ boost::beast::http::verb::get, "/abra/42" }; // Should 404
    gtvr::Response res7;
    router.route(req7, res7);
    std::cout << res7.status << " - " << res7.body << "\n";

    gtvr::Request req8{ boost::beast::http::verb::get, "/api/v1/users/100" };
    gtvr::Response res8;
    router.route(req8, res8);
    std::cout << res8.status << " - " << res8.body << "\n";

    gtvr::Request req9{ boost::beast::http::verb::get, "/api/v1/admin/dashboard" };
    gtvr::Response res9;
    router.route(req9, res9);
    std::cout << res9.status << " - " << res9.body << "\n";
    */

    return 0;
}

TEST_CASE("Testing router", "[router_regex]") {
    REQUIRE(router_func(1) == 0);
}
