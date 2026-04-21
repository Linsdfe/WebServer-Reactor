math.randomseed(os.time())
function request()
    local r = math.random(100000, 9999999)
    local body = "username=bench_"..r.."&password=123456"
    wrk.method = "POST"
    wrk.body = body
    wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
    return wrk.format()
end
