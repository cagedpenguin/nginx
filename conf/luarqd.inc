    location ~ /(.*)
    {
      include proxy.conf;
      proxy_intercept_errors  on;

      #Rate Limiting
      #limit_req zone=one burst=20 nodelay;
      #limit_req_status 429;

      # The configuration area for the catch-all area
      access_log /var/log/nginx/luarqd_access.log main;
      error_log /var/log/nginx/luarqd_error.log debug;

      error_page   400 404               /404.html;
      error_page   500 502 503 504       /500.html;

      # Fix trailing slashes / doing this breaks payment portal. payment id's come in as /?id,
      # Nginx isn't smart enough to recognize the parameters after the last forwardslash to realize
      # the payment portal request should not be stripslashed.
      #rewrite ^/(.*)/$ /$1 permanent;

      #LUARQD - Please use /etc/init.d/nginx_luarqd_ctl to restart/reload nginx.
      set $proxy "";
      rewrite_by_lua_block {
        -- LOAD MEMCACHED
        local memcached = require "resty.memcached"
        local memc, err = memcached:new()
        local memc2, err = memcached:new()
        if not memc then
          ngx.log(ngx.STDERR, "failed to instantiate memc: ", err)
          return
        end
        if not memc2 then
          ngx.log(ngx.STDERR, "failed to instantiate memc2: ", err)
          return
        end

        memc:set_timeout(1000) -- 1 sec
        memc2:set_timeout(1000) -- 1 sec

        -- CONN 1
        local ok, err = memc:connect("127.0.0.1", 11211)
        if not ok then
          ngx.log(ngx.STDERR, "failed to connect: ", err)
          return
        end

        -- CONN 2
        local ok2, err = memc2:connect("127.0.0.1", 11212)
        if not ok2 then
          ngx.log(ngx.STDERR, "failed to connect: ", err)
          return
        end

        -- FIRST TEST REDIRECT THEN PROXY_PASS
        local ruri = ngx.var.request_uri
        ruri = ruri:sub(2)
        -- ngx.log(ngx.STDERR, "DEBUG 1: ", ruri)
        local sub = ruri:match("([a-zA-Z0-9._-]+)/")
        -- ngx.log(ngx.STDERR, "DEBUG 2: ", sub)

        local resR, flagsR, errR = memc:get(ruri)
        local resSS, flagsSS, errSS = memc:get(sub)
        local resP, flagsP, errP = memc2:get(sub)
        local resT, flagsT, errT = memc2:get(ruri)
        --- ngx.log(ngx.STDERR, "Redirect Debug: ", resR, errR)
        --- ngx.log(ngx.STDERR, "Proxy Debug Outside: ", resP, " ", errP)
        --- ngx.log(ngx.STDERR, "Proxy Debut Terminus: ", resT, " ", errT)
        if resR then
          -- ngx.log(ngx.STDERR, "Redirect: ", resR)
          return ngx.redirect(resR, 301)
        end
        if resSS then
          -- ngx.log(ngx.STDERR, "Redirect: ", resSS)
          return ngx.redirect(resSS, 301)
        end
        if resT then
          -- ngx.log(ngx.STDERR, "Redirect local-page: ", resT)
          return ngx.redirect("/local-page/"..ruri, 301)
        end

        -- PROXY_PASS
        if resP then
          -- ngx.log(ngx.STDERR, "Proxy Pass External: ", resP.." / "..ruri)
          ngx.header["content-type"] = "text/html"
          ngx.var.proxy = resP
        end

        -- GRACEFULLY ERROR
        if not resR and not resSS and not resT and not resP then
          ngx.status = 404
          return ngx.exit(ngx.HTTP_NOT_FOUND)
        end
      }
      #proxy_ssl_server_name on;
      #proxy_set_header Host $host;
      proxy_pass https://$proxy;
    }

