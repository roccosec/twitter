#pragma once

namespace tweets {

inline milliseconds timestamp_ms(const Tweet& tw) {
    auto& tweet = tw.data->tweet;
    auto t = milliseconds(stoll(tweet["timestamp_ms"].get<string>()));
    return t;
}

auto isEndOfTweet = [](const string& s){
    if (s.size() < 2) return false;
    auto it0 = s.begin() + (s.size() - 2);
    auto it1 = s.begin() + (s.size() - 1);
    return *it0 == '\r' && *it1 == '\n';
};

inline auto parsetweets(observe_on_one_worker worker, observe_on_one_worker tweetthread) -> function<observable<Tweet>(observable<string>)> {
    return [=](observable<string> chunks) -> observable<Tweet> {
        // create strings split on \r
        auto strings = chunks |
            concat_map([](const string& s){
                auto splits = split(s, "\r\n");
                return iterate(move(splits));
            }) |
            filter([](const string& s){
                return !s.empty();
            }) |
            publish() |
            ref_count();

        // filter to last string in each line
        auto closes = strings |
            filter(isEndOfTweet) |
            rxo::map([](const string&){return 0;});

        // group strings by line
        auto linewindows = strings |
            window_toggle(closes | start_with(0), [=](int){return closes;});

        // reduce the strings for a line into one string
        auto lines = linewindows |
            flat_map([](const observable<string>& w) {
                return w | start_with<string>("") | sum();
            });

        int count = 0;
        return lines |
            filter([](const string& s){
                return s.size() > 2 && s.find_first_not_of("\r\n") != string::npos;
            }) | 
            group_by([count](const string&) mutable -> int {
                return ++count % std::thread::hardware_concurrency();}) |
            rxo::map([=](observable<string> shard) {
                return shard | 
                    observe_on(worker) | 
                    rxo::map([](const string& line){
                        return Tweet(json::parse(line));
                    }) |
                    as_dynamic();
            }) |
            merge(tweetthread);
    };
}

inline auto onlytweets() -> function<observable<Tweet>(observable<Tweet>)> {
    return [](observable<Tweet> s){
        return s | filter([](const Tweet& tw){
            auto& tweet = tw.data->tweet;
            return !!tweet.count("timestamp_ms");
        });
    };
}

enum class errorcodeclass {
    Invalid,
    TcpRetry,
    ErrorRetry,
    StatusRetry,
    RateLimited
};

inline errorcodeclass errorclassfrom(const http_exception& ex) {
    switch(ex.code()) {
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_BAD_CONTENT_ENCODING:
        case CURLE_REMOTE_FILE_NOT_FOUND:
            return errorcodeclass::ErrorRetry;
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
            return errorcodeclass::TcpRetry;
        default:
            if (ex.code() == CURLE_HTTP_RETURNED_ERROR || ex.httpStatus() > 200) {
                if (ex.httpStatus() == 420) {
                    return errorcodeclass::RateLimited;
                } else if (ex.httpStatus() == 404 ||
                    ex.httpStatus() == 406 ||
                    ex.httpStatus() == 413 ||
                    ex.httpStatus() == 416) {
                    return errorcodeclass::Invalid;
                }
            }
    };
    return errorcodeclass::StatusRetry;
}

auto filechunks = [](observe_on_one_worker tweetthread, string filepath) {
    return observable<>::create<string>([=](subscriber<string> out){

        auto values = make_tuple(ifstream{filepath}, string{});
        auto state = make_shared<decltype(values)>(move(values));

        // creates a worker whose lifetime is the same as this subscription
        auto coordinator = tweetthread.create_coordinator(out.get_subscription());

        auto controller = coordinator.get_worker();

        auto producer = [out, state](const rxsc::schedulable& self) {

            if (!out.is_subscribed()) {
                // terminate loop
                return;
            }

            if (getline(get<0>(*state), get<1>(*state)))
            {
                get<1>(*state)+="\r\n";
                out.on_next(get<1>(*state));
            } else {
                out.on_completed();
                return;
            }

            // tail recurse this same action to continue loop
            self();
        };

        //controller.schedule_periodically(controller.now(), milliseconds(100), coordinator.act(producer));
        controller.schedule(coordinator.act(producer));
    });
};

auto twitter_stream_reconnection = [](observe_on_one_worker tweetthread){
    return [=](observable<string> chunks){
        return chunks |
            // https://dev.twitter.com/streaming/overview/connecting
            timeout(seconds(90), tweetthread) |
            on_error_resume_next([=](std::exception_ptr ep) -> observable<string> {
                try {rethrow_exception(ep);}
                catch (const http_exception& ex) {
                    cerr << ex.what() << endl;
                    switch(errorclassfrom(ex)) {
                        case errorcodeclass::TcpRetry:
                            cerr << "reconnecting after TCP error" << endl;
                            return observable<>::empty<string>();
                        case errorcodeclass::ErrorRetry:
                            cerr << "error code (" << ex.code() << ") - ";
                        case errorcodeclass::StatusRetry:
                            cerr << "http status (" << ex.httpStatus() << ") - waiting to retry.." << endl;
                            return observable<>::timer(seconds(5), tweetthread) | stringandignore();
                        case errorcodeclass::RateLimited:
                            cerr << "rate limited - waiting to retry.." << endl;
                            return observable<>::timer(minutes(1), tweetthread) | stringandignore();
                        case errorcodeclass::Invalid:
                            cerr << "invalid request - exit" << endl;
                        default:
                            return observable<>::error<string>(ep, tweetthread);
                    };
                }
                catch (const timeout_error& ex) {
                    cerr << "reconnecting after timeout" << endl;
                    return observable<>::empty<string>();
                }
                catch (const exception& ex) {
                    cerr << ex.what() << endl;
                    terminate();
                }
                catch (...) {
                    cerr << "unknown exception - not derived from std::exception" << endl;
                    terminate();
                }
                return observable<>::error<string>(ep, tweetthread);
            }) |
            repeat(0);
    };
};

auto twitterrequest = [](observe_on_one_worker tweetthread, ::rxcurl::rxcurl factory, string URL, string method, string CONS_KEY, string CONS_SEC, string ATOK_KEY, string ATOK_SEC){

    return observable<>::defer([=](){

        string url;
        {
            char* signedurl = nullptr;
            RXCPP_UNWIND_AUTO([&](){
                if (!!signedurl) {
                    free(signedurl);
                }
            });
            signedurl = oauth_sign_url2(
                URL.c_str(), NULL, OA_HMAC, method.c_str(),
                CONS_KEY.c_str(), CONS_SEC.c_str(), ATOK_KEY.c_str(), ATOK_SEC.c_str()
            );
            url = signedurl;
        }

        return factory.create(http_request{url, method}) |
            rxo::map([](http_response r){
                return r.body.chunks;
            }) |
            merge(tweetthread);
    }) |
    twitter_stream_reconnection(tweetthread);
};


}
