(function () {
    // A cached loader must also remain inert on an invitation page.
    if (/^\/(?:(?:en|ru)\/)?join\/?$/.test(location.pathname)) return;

    (function(m,e,t,r,i,k,a){
        m[i]=m[i]||function(){(m[i].a=m[i].a||[]).push(arguments)};
        m[i].l=1*new Date();
        for (var j=0;j<document.scripts.length;j++){
            if(document.scripts[j].src===r){return;}
        }
        k=e.createElement(t);
        a=e.getElementsByTagName(t)[0];
        k.async=1;
        k.src=r;
        a.parentNode.insertBefore(k,a);
    })(window,document,'script','https://mc.yandex.ru/metrika/tag.js?id=112808244','ym');

    ym(112808244,'init',{
        ssr:true,
        webvisor:true,
        clickmap:true,
        ecommerce:"dataLayer",
        referrer:document.referrer,
        url:location.origin + location.pathname + location.search,
        accurateTrackBounce:true,
        trackLinks:true
    });
})();
