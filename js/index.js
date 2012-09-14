$(function(){
    var converter = new Showdown.converter();

    var LoadDiaryByPath = function(path){
        var fp = new File(path);
        var content = "";
        if(fp.open("r")){
            content = fp.read(fp.getLength());
            fp.close();
        }
        return content;
    }

    $("#blogcontent").append(converter.makeHtml(LoadDiaryByPath("../post/20120914-github建个人blog")));
});
