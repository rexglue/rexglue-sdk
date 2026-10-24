(function () {
  var MERMAID_URL = "https://cdn.jsdelivr.net/npm/mermaid@11.4.0/dist/mermaid.min.js";
  var icons = { default: "assets/favicon-green.png", slate: "assets/favicon-blue.png" };
  var counter = 0;
  var STATE_LAYOUT = '%%{init: {"flowchart": {"nodeSpacing": 80, "rankSpacing": 110}}}%%\n';

  function siteBase() {
    var scripts = document.querySelectorAll("script[src*='javascripts/extra.js']");
    if (scripts.length) {
      return scripts[0].src.replace(/javascripts\/extra\.js.*$/, "");
    }
    return "";
  }

  function scheme() {
    return document.body.getAttribute("data-md-color-scheme") || "default";
  }

  function applyFavicon() {
    var link = document.querySelector("link[rel~='icon']");
    if (!link) {
      link = document.createElement("link");
      link.rel = "icon";
      document.head.appendChild(link);
    }
    link.type = "image/png";
    link.href = siteBase() + (icons[scheme()] || icons.default);
  }

  function readVars() {
    var style = getComputedStyle(document.body);
    function v(name) {
      return style.getPropertyValue(name).trim();
    }
    var vars = {
      background: v("--rex-d-bg"),
      primaryColor: v("--rex-d-node-bg"),
      primaryTextColor: v("--rex-d-fg"),
      primaryBorderColor: v("--rex-d-border"),
      secondaryColor: v("--rex-d-alt-bg"),
      tertiaryColor: v("--rex-d-alt-bg"),
      lineColor: v("--rex-d-line"),
      textColor: v("--rex-d-fg"),
      mainBkg: v("--rex-d-node-bg"),
      nodeBorder: v("--rex-d-border"),
      nodeTextColor: v("--rex-d-fg"),
      clusterBkg: v("--rex-d-alt-bg"),
      clusterBorder: v("--rex-d-border"),
      titleColor: v("--rex-d-fg"),
      edgeLabelBackground: v("--rex-d-bg"),
      noteBkgColor: v("--rex-d-alt-bg"),
      noteTextColor: v("--rex-d-fg"),
      noteBorderColor: v("--rex-d-border"),
      actorBkg: v("--rex-d-node-bg"),
      actorBorder: v("--rex-d-border"),
      actorTextColor: v("--rex-d-fg"),
      actorLineColor: v("--rex-d-lifeline"),
      signalColor: v("--rex-d-line"),
      signalTextColor: v("--rex-d-fg"),
      labelBoxBkgColor: v("--rex-d-tag-bg"),
      labelBoxBorderColor: v("--rex-d-border"),
      labelTextColor: v("--rex-d-tag-fg"),
      loopTextColor: v("--rex-d-fg"),
      activationBkgColor: v("--rex-d-alt-bg"),
      activationBorderColor: v("--rex-d-border"),
      sequenceNumberColor: v("--rex-d-tag-fg"),
      classText: v("--rex-d-fg"),
      stateBkg: v("--rex-d-node-bg"),
      stateLabelColor: v("--rex-d-fg"),
      transitionColor: v("--rex-d-line"),
      transitionLabelColor: v("--rex-d-fg"),
      labelColor: v("--rex-d-fg"),
      compositeBackground: v("--rex-d-alt-bg"),
      compositeBorder: v("--rex-d-border"),
      compositeTitleBackground: v("--rex-d-node-bg"),
      attributeBackgroundColorOdd: v("--rex-d-node-bg"),
      attributeBackgroundColorEven: v("--rex-d-alt-bg"),
      taskBkgColor: v("--rex-d-node-bg"),
      taskBorderColor: v("--rex-d-border"),
      taskTextColor: v("--rex-d-fg"),
      taskTextLightColor: v("--rex-d-fg"),
      taskTextDarkColor: v("--rex-d-tag-fg"),
      taskTextOutsideColor: v("--rex-d-fg"),
      activeTaskBkgColor: v("--rex-d-alt-bg"),
      activeTaskBorderColor: v("--rex-d-border"),
      doneTaskBkgColor: v("--rex-d-tag-bg"),
      doneTaskBorderColor: v("--rex-d-border"),
      gridColor: v("--rex-d-lifeline"),
      sectionBkgColor: v("--rex-d-alt-bg"),
      altSectionBkgColor: v("--rex-d-bg"),
      sectionBkgColor2: v("--rex-d-alt-bg"),
      todayLineColor: v("--rex-d-border"),
      cScale0: v("--rex-d-mm-root"),
      cScaleLabel0: v("--rex-d-mm-root-fg"),
      git0: v("--rex-d-mm-root"),
      gitBranchLabel0: v("--rex-d-mm-root-fg"),
      fontFamily: style.getPropertyValue("--md-text-font-family").trim() || "Roboto, sans-serif",
      fontSize: "16px"
    };
    vars.cScaleInv0 = vars.cScale0;
    for (var i = 1; i < 12; i++) {
      vars["cScale" + i] = v("--rex-d-mm-" + (((i - 1) % 4) + 1));
      vars["cScaleLabel" + i] = v("--rex-d-mm-fg");
      vars["cScaleInv" + i] = vars["cScale" + i];
    }
    return vars;
  }

  function loadMermaid(callback) {
    if (window.mermaid) {
      callback();
      return;
    }
    var script = document.createElement("script");
    script.src = MERMAID_URL;
    script.onload = callback;
    document.head.appendChild(script);
  }

  function renderAll() {
    var blocks = document.querySelectorAll("pre.diagram");
    if (!blocks.length) {
      return;
    }
    loadMermaid(function () {
      window.mermaid.initialize({
        startOnLoad: false,
        theme: "base",
        securityLevel: "strict",
        themeVariables: readVars(),
        markdownAutoWrap: false,
        flowchart: {
          htmlLabels: true,
          useMaxWidth: false,
          wrappingWidth: 400,
          nodeSpacing: 45,
          rankSpacing: 55,
          padding: 14
        },
        sequence: {
          useMaxWidth: false,
          wrap: false,
          width: 130,
          height: 46,
          actorMargin: 28,
          messageMargin: 38,
          boxMargin: 10,
          mirrorActors: false
        },
        state: { useMaxWidth: false },
        class: { useMaxWidth: false },
        er: { useMaxWidth: false },
        block: { useMaxWidth: false },
        gantt: { useMaxWidth: false, useWidth: 800, leftPadding: 100, barHeight: 22, barGap: 6 },
        mindmap: { useMaxWidth: false }
      });
      blocks.forEach(function (pre) {
        if (!pre.dataset.source) {
          pre.dataset.source = pre.textContent;
        }
        var view = pre.nextElementSibling;
        if (!view || !view.classList.contains("diagram-view")) {
          view = document.createElement("div");
          view.className = "diagram-view";
          pre.parentNode.insertBefore(view, pre.nextSibling);
        }
        counter += 1;
        var source = pre.dataset.source;
        if (/^\s*stateDiagram/.test(source)) {
          source = STATE_LAYOUT + source;
        }
        window.mermaid
          .render("rex-diagram-" + counter, source)
          .then(function (result) {
            view.innerHTML = result.svg;
            pre.hidden = true;
          })
          .catch(function () {
            pre.hidden = false;
            view.innerHTML = "";
          });
      });
    });
  }

  function start() {
    applyFavicon();
    renderAll();
    new MutationObserver(function () {
      applyFavicon();
      renderAll();
    }).observe(document.body, { attributes: true, attributeFilter: ["data-md-color-scheme"] });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", start);
  } else {
    start();
  }
})();
