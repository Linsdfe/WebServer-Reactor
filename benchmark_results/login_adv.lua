local users = {
    {u = "testuser", p = "testpass"},
    {u = "user1", p = "pass1"}, {u = "user2", p = "pass2"},
    {u = "user3", p = "pass3"}, {u = "user4", p = "pass4"},
    {u = "user5", p = "pass5"}, {u = "user6", p = "pass6"},
    {u = "user7", p = "pass7"}, {u = "user8", p = "pass8"},
    {u = "user9", p = "pass9"}, {u = "user10", p = "pass10"}
}
local index = 0
local user_count = 11
function request()
    index = index + 1
    if index > user_count then index = 1 end
    local cred = users[index]
    local body = "username=" .. cred.u .. "&password=" .. cred.p
    wrk.method = "POST"
    wrk.body = body
    wrk.headers["Content-Type"] = "application/x-www-form-urlencoded"
    return wrk.format()
end
