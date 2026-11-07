// MSX PICOVERSE PROJECT
// pva_news_cat.c - PicoVerse Audio Engine: the news reader's catalogue of
// feeds by topic. Each feed has a short code that PICOAMP.INI keeps in
// [news] feeds=code,code,... (the favourites), so the settings stay small.
//
// Gathered from the lists at opentweet.io/news-rss-feeds, feeder.co (popular
// and useful RSS feeds), rssly.io/feeds/news and rss.feedspot.com, plus the
// feeds of NOS and a few Dutch, European and retro computing sites.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include "pva_internal.h"

const pva_news_topic_t pva_news_topics[] = {
    { "World news" },
    { "UK and Europe" },
    { "Nederland" },
    { "Deutsch, Francais, Espanol" },
    { "US news" },
    { "Business" },
    { "Technology" },
    { "Science and space" },
    { "Sports" },
    { "Games" },
    { "MSX and retro computing" },
    { NULL }
};

// code, name, URL (http where the site still serves it: less work for the ESP-01)
const pva_news_feed_t pva_news_cat[] = {
    // 0 World news
    { 0, "bbcw",    "BBC News, World",            "http://feeds.bbci.co.uk/news/world/rss.xml" },
    { 0, "bbc",     "BBC News, top stories",      "http://feeds.bbci.co.uk/news/rss.xml" },
    { 0, "alj",     "Al Jazeera",                 "https://www.aljazeera.com/xml/rss/all.xml" },
    { 0, "dw",      "DW (English)",               "https://rss.dw.com/xml/rss-en-all" },
    { 0, "guardw",  "The Guardian, World",        "https://www.theguardian.com/world/rss" },
    { 0, "nytw",    "New York Times, World",      "https://rss.nytimes.com/services/xml/rss/nyt/World.xml" },
    { 0, "skyw",    "Sky News, World",            "https://feeds.skynews.com/feeds/rss/world.xml" },
    { 0, "npr",     "NPR News",                   "https://feeds.npr.org/1001/rss.xml" },
    { 0, "f24",     "France 24 (English)",        "https://www.france24.com/en/rss" },
    { 0, "cbc",     "CBC News",                   "https://www.cbc.ca/webfeed/rss/rss-topstories" },
    { 0, "abcau",   "ABC News (Australia)",       "https://www.abc.net.au/news/feed/45910/rss.xml" },
    { 0, "scmp",    "South China Morning Post",   "https://www.scmp.com/rss/91/feed/" },
    { 0, "toi",     "The Times of India",         "https://timesofindia.indiatimes.com/rssfeedstopstories.cms" },
    { 0, "gnw",     "Google News, World",         "https://news.google.com/rss/headlines/section/topic/WORLD?hl=en-US&gl=US&ceid=US:en" },
    // 1 UK and Europe
    { 1, "bbcuk",   "BBC News, UK",               "http://feeds.bbci.co.uk/news/uk/rss.xml" },
    { 1, "guarduk", "The Guardian, UK",           "https://www.theguardian.com/uk/rss" },
    { 1, "indep",   "The Independent",            "https://www.independent.co.uk/rss" },
    { 1, "sky",     "Sky News",                   "https://feeds.skynews.com/feeds/rss/home.xml" },
    { 1, "rte",     "RTE News",                   "https://www.rte.ie/feeds/rss/?index=/news/" },
    { 1, "euron",   "Euronews",                   "https://www.euronews.com/rss" },
    { 1, "polieu",  "Politico Europe",            "https://www.politico.eu/feed/" },
    { 1, "lmen",    "Le Monde in English",        "https://www.lemonde.fr/en/rss/une.xml" },
    { 1, "spint",   "Der Spiegel International",  "https://www.spiegel.de/international/index.rss" },
    // 2 Nederland
    { 2, "nos",     "NOS Nieuws",                 "https://feeds.nos.nl/nosnieuwsalgemeen" },
    { 2, "nosbi",   "NOS Binnenland",             "https://feeds.nos.nl/nosnieuwsbinnenland" },
    { 2, "nosbu",   "NOS Buitenland",             "https://feeds.nos.nl/nosnieuwsbuitenland" },
    { 2, "nospol",  "NOS Politiek",               "https://feeds.nos.nl/nosnieuwspolitiek" },
    { 2, "noseco",  "NOS Economie",               "https://feeds.nos.nl/nosnieuwseconomie" },
    { 2, "nostech", "NOS Tech",                   "https://feeds.nos.nl/nosnieuwstech" },
    { 2, "nosopm",  "NOS Opmerkelijk",            "https://feeds.nos.nl/nosnieuwsopmerkelijk" },
    { 2, "noscul",  "NOS Cultuur en media",       "https://feeds.nos.nl/nosnieuwscultuurenmedia" },
    { 2, "nuus",    "Nieuwsuur",                  "https://feeds.nos.nl/nieuwsuuralgemeen" },
    { 2, "jeugd",   "NOS Jeugdjournaal",          "https://feeds.nos.nl/jeugdjournaal" },
    { 2, "nu",      "NU.nl",                      "https://www.nu.nl/rss/Algemeen" },
    { 2, "tweak",   "Tweakers",                   "https://tweakers.net/feeds/mixed.xml" },
    { 2, "tweakn",  "Tweakers, nieuws",           "https://tweakers.net/feeds/nieuws.xml" },
    // 3 Deutsch, Francais, Espanol, Italiano
    { 3, "tages",   "tagesschau",                 "https://www.tagesschau.de/index~rss2.xml" },
    { 3, "spiegel", "Der Spiegel",                "https://www.spiegel.de/schlagzeilen/index.rss" },
    { 3, "dwde",    "DW (Deutsch)",               "https://rss.dw.com/xml/rss-de-all" },
    { 3, "lemonde", "Le Monde, a la une",         "https://www.lemonde.fr/rss/une.xml" },
    { 3, "finfo",   "franceinfo, les titres",     "https://www.francetvinfo.fr/titres.rss" },
    { 3, "f24fr",   "France 24 (Francais)",       "https://www.france24.com/fr/rss" },
    { 3, "elpais",  "El Pais, portada",           "https://feeds.elpais.com/mrss-s/pages/ep/site/elpais.com/portada" },
    { 3, "mundo",   "BBC Mundo",                  "http://feeds.bbci.co.uk/mundo/rss.xml" },
    { 3, "ansa",    "ANSA, ultima ora",           "https://www.ansa.it/sito/notizie/topnews/topnews_rss.xml" },
    // 4 US news
    { 4, "nyt",     "New York Times",             "https://rss.nytimes.com/services/xml/rss/nyt/HomePage.xml" },
    { 4, "cbs",     "CBS News",                   "https://www.cbsnews.com/latest/rss/main" },
    { 4, "nbc",     "NBC News",                   "https://feeds.nbcnews.com/nbcnews/public/news" },
    { 4, "pbs",     "PBS News Hour",              "https://www.pbs.org/newshour/feeds/rss/headlines" },
    { 4, "abc",     "ABC News",                   "https://abcnews.go.com/abcnews/topstories" },
    { 4, "lat",     "Los Angeles Times",          "https://www.latimes.com/index.rss" },
    { 4, "time",    "TIME",                       "https://time.com/feed/" },
    { 4, "propub",  "ProPublica",                 "https://www.propublica.org/feeds/propublica/main" },
    { 4, "atlan",   "The Atlantic",               "https://www.theatlantic.com/feed/all/" },
    { 4, "vox",     "Vox",                        "https://www.vox.com/rss/index.xml" },
    { 4, "politi",  "Politico",                   "https://rss.politico.com/politics-news.xml" },
    { 4, "hill",    "The Hill",                   "https://thehill.com/news/feed/" },
    { 4, "nprpol",  "NPR Politics",               "https://feeds.npr.org/1014/rss.xml" },
    // 5 Business
    { 5, "bbcbiz",  "BBC News, Business",         "http://feeds.bbci.co.uk/news/business/rss.xml" },
    { 5, "wsjm",    "WSJ, Markets",               "https://feeds.content.dowjones.io/public/rss/RSSMarketsMain" },
    { 5, "mw",      "MarketWatch",                "https://feeds.content.dowjones.io/public/rss/mw_topstories" },
    { 5, "cnbc",    "CNBC, top news",             "https://www.cnbc.com/id/100003114/device/rss/rss.html" },
    { 5, "ft",      "Financial Times",            "https://www.ft.com/rss/home/international" },
    { 5, "econfin", "The Economist, Finance",     "https://www.economist.com/finance-and-economics/rss.xml" },
    { 5, "fortune", "Fortune",                    "https://fortune.com/feed/fortune-feeds/?id=3230629" },
    { 5, "bi",      "Business Insider",           "https://feeds.businessinsider.com/custom/all" },
    { 5, "fastco",  "Fast Company",               "https://www.fastcompany.com/latest/rss" },
    // 6 Technology
    { 6, "ars",     "Ars Technica",               "https://feeds.arstechnica.com/arstechnica/index" },
    { 6, "verge",   "The Verge",                  "https://www.theverge.com/rss/index.xml" },
    { 6, "tc",      "TechCrunch",                 "https://techcrunch.com/feed/" },
    { 6, "wired",   "Wired",                      "https://www.wired.com/feed/rss" },
    { 6, "engadg",  "Engadget",                   "https://www.engadget.com/rss.xml" },
    { 6, "hn",      "Hacker News, front page",    "https://hnrss.org/frontpage" },
    { 6, "techme",  "Techmeme",                   "https://www.techmeme.com/feed.xml" },
    { 6, "mittr",   "MIT Technology Review",      "https://www.technologyreview.com/feed/" },
    { 6, "ieee",    "IEEE Spectrum",              "https://feeds.feedburner.com/IeeeSpectrumFullText" },
    { 6, "bleep",   "BleepingComputer",           "https://www.bleepingcomputer.com/feed/" },
    { 6, "krebs",   "Krebs on Security",          "https://krebsonsecurity.com/feed/" },
    { 6, "bbctech", "BBC News, Technology",       "http://feeds.bbci.co.uk/news/technology/rss.xml" },
    // 7 Science and space
    { 7, "nasa",    "NASA news releases",         "https://www.nasa.gov/news-release/feed/" },
    { 7, "scid",    "ScienceDaily",               "https://www.sciencedaily.com/rss/all.xml" },
    { 7, "phys",    "Phys.org",                   "https://phys.org/rss-feed/" },
    { 7, "newsci",  "New Scientist",              "https://www.newscientist.com/feed/" },
    { 7, "space",   "Space.com",                  "https://www.space.com/feeds/all" },
    { 7, "quanta",  "Quanta Magazine",            "https://www.quantamagazine.org/feed/" },
    { 7, "sciam",   "Scientific American",        "https://www.scientificamerican.com/platform/syndication/rss/" },
    { 7, "nature",  "Nature",                     "https://www.nature.com/nature.rss" },
    { 7, "bbcsci",  "BBC Science, environment",   "http://feeds.bbci.co.uk/news/science_and_environment/rss.xml" },
    // 8 Sports
    { 8, "bbcsp",   "BBC Sport",                  "http://feeds.bbci.co.uk/sport/rss.xml" },
    { 8, "bbcfb",   "BBC Sport, Football",        "http://feeds.bbci.co.uk/sport/football/rss.xml" },
    { 8, "nossp",   "NOS Sport",                  "https://feeds.nos.nl/nossportalgemeen" },
    { 8, "nosvb",   "NOS Voetbal",                "https://feeds.nos.nl/nosvoetbal" },
    { 8, "nosf1",   "NOS Formule 1",              "https://feeds.nos.nl/nossportformule1" },
    { 8, "nosws",   "NOS Wielrennen",             "https://feeds.nos.nl/nossportwielrennen" },
    { 8, "guardsp", "The Guardian, Sport",        "https://www.theguardian.com/uk/sport/rss" },
    { 8, "skysp",   "Sky Sports",                 "https://www.skysports.com/rss/12040" },
    { 8, "espn",    "ESPN, top headlines",        "https://www.espn.com/espn/rss/news" },
    { 8, "f1",      "Formula 1",                  "https://www.formula1.com/en/latest/all.xml" },
    { 8, "cric",    "ESPNcricinfo",               "https://www.cricinfo.com/rss/content/story/feeds/0.xml" },
    // 9 Games
    { 9, "ign",     "IGN",                        "https://www.ign.com/rss/articles/feed" },
    { 9, "gspot",   "GameSpot",                   "https://www.gamespot.com/feeds/mashup/" },
    { 9, "pcg",     "PC Gamer",                   "https://www.pcgamer.com/rss/" },
    { 9, "rps",     "Rock Paper Shotgun",         "https://www.rockpapershotgun.com/feed" },
    { 9, "eurog",   "Eurogamer",                  "https://www.eurogamer.net/feed" },
    { 9, "nlife",   "Nintendo Life",              "https://www.nintendolife.com/feeds/latest" },
    { 9, "kotaku",  "Kotaku",                     "https://kotaku.com/feed" },
    // 10 MSX and retro computing
    { 10, "mrc",    "MSX Resource Center",        "https://www.msx.org/feed/news" },
    { 10, "timext", "Time Extension",             "https://www.timeextension.com/feeds/latest" },
    { 10, "hackad", "Hackaday",                   "https://hackaday.com/blog/feed/" },
    { 10, "vinold", "Vintage is the New Old",     "https://www.vintageisthenewold.com/feed" },
    { 10, "hnrss",  "Hacker News, best",          "https://hnrss.org/newest?points=100" },
    { 0xFF, NULL, NULL, NULL }
};

// the favourites when PICOAMP.INI names none
const char pva_news_default_favs[] = "bbcw,alj,dw,guardw,nos";
