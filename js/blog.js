
var JoyXu = JoyXu || {
    Blog:{}
}

JoyXu.Blog = function(){
    this.host = "post/" ; //"file:///source/blog/post/";
    this.indexPage = encodeURI(this.host + "index.json");
    this.col = 3;
    this.tips = [];
    this.showTipDiv = "";

}

JoyXu.Blog.prototype.initPerson = function(parentid){

}

JoyXu.Blog.prototype.setTipDetailDiv = function(parentid){
    this.showTipDiv = parentid;
    $("#" + this.showTipDiv).click(function(){
        $("#" + parentid).hide();
    });
}

JoyXu.Blog.prototype.initTips = function(parentid){
    var that = this;
    $.ajax({
        url: that.indexPage ,
        dataType: "html",
        error: function(jqXHR, textStatus, errorThrown){
            alert(textStatus);
            alert( jqXHR.responseText);
        },
        success: function(data){
            var tips = eval(data);
            var index = 0;
            var tip;
            var html = "";
            var param = {};
            for( index = 0 ; index < tips.length ; index++){
                param.id = index;
                param.title = tips[index].title;
                param.content = tips[index].content;
                param.date = tips[index].date;
                param.category = tips[index].category;
                tip = new JoyXu.Tip(param);
                that.tips.push(tip);
                if( 0 == index % that.col ){
                    html += "<ul class='tiprow'>";
                }
                html += "<li class='tip'>" + tip.getShortTip() + "</li>";
                if( that.col - 1 == index % that.col){
                    html += "</ul>"
                }
            }
            if( that.col - 1 > index % that.col){
                html += "</ul>"
            }
            $("#" + parentid).append(html);
            $("#" + parentid + "> ul").each(function(i){
                var row = i;
                $(this).find(">li").each(function(j){
                $(this).click(function(){
                        that.setClickOnTip(row*that.col + j);
                    });               
                });
            });
        }
    });
}

JoyXu.Blog.prototype.setClickOnTip = function(index){
    var tip = this.tips[index];
    var that = this;
    $("#" + this.showTipDiv).empty();
    $.ajax({
        url: tip.path,
        dataType: "html",
        success: function(data){
            var converter = new Showdown.converter();
            $("#" + that.showTipDiv).append(converter.makeHtml(data));
            $("#" + that.showTipDiv).css({ 
                position: 'absolute', 
                'z-index': 10000, 
                width: $("body").width(), 
            }); 
            $("#" + that.showTipDiv).show();
        }
    });
}

JoyXu.Tip = function(param){
    this.id = param.id || "";
    this.title = param.title || "";
    this.content = param.content || "";
    this.date = param.date || "";
    this.category = param.category || "other";
    this.host = "post/"; //"file:///source/blog/post/";    
    this.path = encodeURI(this.host + this.title);
}

JoyXu.Tip.prototype.getShortTip = function(){
    var html = "<article><header></header><a><h3>" + this.title + "</h3></a><footer>";
    for(var i in this.category){
        html += "<a>" + this.category[i]+ "</a>";
    }
    html += "</footer></article>";
    return html;
}
