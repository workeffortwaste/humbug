from PIL import Image, ImageDraw, ImageColor
glyphs=dict(line.split() for line in '''
A 0E11111F111111
B 1E11111E11111E
C 0F10101010100F
D 1E11111111111E
E 1F10101E10101F
F 1F10101E101010
G 0F10101711110F
H 1111111F111111
I 0E04040404040E
J 0702020212120C
K 11121418141211
L 1010101010101F
M 111B1515111111
N 11191513111111
O 0E11111111110E
P 1E11111E101010
Q 0E11111115120D
R 1E11111E141211
S 0F10100E01011E
T 1F040404040404
U 1111111111110E
V 11111111110A04
W 11111115151B11
X 11110A040A1111
Y 11110A04040404
Z 1F01020408101F
0 0E11131519110E
1 040C040404040E
2 0E11010204081F
3 1E01010E01011E
4 02060A121F0202
5 1F10101E01011E
6 0E10101E11110E
7 1F010204080808
8 0E11110E11110E
9 0E11110F01010E
! 04040404040004
, 00000000000C08
/ 01010204081010
'''.strip().splitlines())
glyphs[' ']='00000000000000'

glyphs.update({'.':'00000000000C0C',':':'000C0C000C0C00','+':'0004041F040400','?':'0E110102040004','-':'0000001F000000','&':'0C12140815120D', chr(39):'04040800000000','(':'02040808080402',')':'08040202020408','_':'0000000000001F'})

class Screen:
    def __init__(self,palette):
        self.p=palette
        self.im=Image.new("RGB",(320,170),(0,0,0))
        self.d=ImageDraw.Draw(self.im)
    def box(self,b,c):
        x,y,w,h=b
        assert x>=0 and y>=0 and x+w<=320 and y+h<=170
        self.d.rectangle((x,y,x+w-1,y+h-1),fill=self.p[c])
    def round(self,b,c,r=4,outline=None):
        x,y,w,h=b
        self.d.rounded_rectangle((x,y,x+w-1,y+h-1),r,fill=self.p[c],outline=self.p[outline] if outline else None)
    def path(self,points,c,width=1):
        self.d.line(points,fill=self.p[c],width=width)
    def shape(self,points,c):
        self.d.polygon(points,fill=self.p[c])
    def disc(self,x,y,r,c,outline=None):
        self.d.ellipse((x-r,y-r,x+r,y+r),fill=self.p[c],outline=self.p[outline] if outline else None)
    def panel(self,x,y,w,h,body='panel',rim='edge',top='edge',low='under',lift=2,r=4):
        self.round((x+lift,y+lift,w,h),'shadow',r)
        self.round((x,y,w,h),body,r,rim)
        self.path([(x+r,y+1),(x+w-r-1,y+1)],top)
        self.path([(x+w-2,y+r),(x+w-2,y+h-r-1),(x+w-r-1,y+h-2),(x+r,y+h-2)],low)
    def screw(self,x,y):
        self.disc(x,y,2,'shadow')
        self.path([(x-1,y),(x+1,y)],'edge')

tap=dict(bg='#211626',panel='#38263c',well='#2a1c2e',edge='#76535e',under='#482d42',
        shadow='#110c17',ink='#ffebc8',muted='#dfb8c0',green='#b9ddb0',greenHi='#e2f2ba',greenLo='#65866a',
        warn='#f4888c',warnHi='#ffc2ad',warnLo='#9f465e',buttonInk='#211626',active='#f9c57d',
        activeHi='#ffe6b0',activeLo='#a36a48',lip='#a36551',shell='#66454f',shellHi='#cc9273',
        shellLow='#4c303d',paper='#ffebc8',paperShade='#e0af93',tapePink='#e3adba',
        hub='#cda5a7',tape='#7d533f',oxide='#c48e63',redPaper='#a05255',glass='#2e2031')

TAPE_BBOX=(10,30,176,112)
def cassette(q):
    x,y,w,h=TAPE_BBOX
    assert 1.55<=w/h<=1.60
    # 176 × 112 px outer shell, exclusive of its offset shadow.
    q.round((x+3,y+3,w,h),'shadow',7)
    q.round(TAPE_BBOX,'shell',7,'shellLow')
    q.round((x+1,y+1,w-2,h-4),'lip',6,'shellHi')
    q.round((x+4,y+5,w-8,h-11),'shell',5,'shadow')
    q.path([(x+5,y+h-4),(x+w-8,y+h-4),(x+w-4,y+h-8)],'shellLow',2)
    q.round((23,43,150,62),'paperShade',4,'under')
    q.round((25,43,147,59),'paper',3)
    q.box((25,62,147,30),'tapePink')
    q.box((25,94,147,5),'active')
    q.box((25,99,147,2),'lip')
    q.path([(48,57),(159,57)],'paperShade')

    # A single recessed spool window and visible tape packs.
    q.round((38,66,120,25),'shadow',7,'redPaper')
    q.round((41,68,114,20),'glass',6,'shellLow')
    q.box((61,73,74,10),'tape')
    q.box((61,75,74,2),'oxide')
    for xx in (57,139):
        q.disc(xx+1,80,10,'shadow')
        q.disc(xx,78,10,'tape','oxide')
        q.disc(xx,78,6,'hub','shadow')
        q.disc(xx,78,3,'well')
        for dx,dy in ((-4,0),(0,-4),(4,0),(0,4)):
            q.box((xx+dx-1,78+dy-1,3,3),'paperShade')

    # Raised lower tape-head housing, mould lines and flush screw heads.
    q.shape([(49,108),(148,108),(164,133),(33,133)],'shadow')
    q.shape([(51,107),(145,107),(160,132),(36,132)],'shellLow')
    q.path([(37,130),(51,108),(145,108)],'shellHi')
    q.path([(41,132),(155,132)],'shadow')
    q.round((77,120,42,8),'well',2,'shadow')
    q.box((84,122,7,3),'oxide');q.box((106,122,6,3),'oxide')
    q.disc(58,123,3,'shadow','edge');q.disc(138,123,3,'shadow','edge')
    for xx,yy in ((19,38),(177,38),(19,133),(177,133),(98,112)):
        q.screw(xx,yy)

def cassette_blank_label(q):
    q.box((28,45,140,16),'paper')
    q.path([(33,52),(134,52)],'paperShade')
    q.path([(33,58),(134,58)],'paperShade')
    q.box((141,47,22,12),'paperShade')

def cassette_bootleg_scratch(q):
    spans=((40,39),(49,33),(34,48),(33,52),(33,51),(34,48),(32,49),
           (35,47),(33,45),(43,36),(37,40),(36,39),(39,39))
    for y,(x,width) in enumerate(spans,45):
        q.box((x,y,width,1),'cassetteScratch')

def color(s): return ImageColor.getrgb(s if s.startswith('#') else '#'+s)
def blend(a,b,t):
    return '#%02x%02x%02x'%tuple(round((1-t)*x+t*y) for x,y in zip(color(a),color(b)))
def luminance(s):
    return sum(w*(v/255/12.92 if v<=10 else ((v/255+.055)/1.055)**2.4) for v,w in zip(color(s),(.2126,.7152,.0722)))
def contrast(a,b):
    x,y=sorted((luminance(a),luminance(b)))
    return (y+.05)/(x+.05)
def ink_for(c):
    return max(('#10131b','#fffbef'),key=lambda level:contrast(level,c))

def build_palette(raw):
    bg,panel,ink,shell,lip,paper,stripe,active,warn,green=('#'+v for v in raw.split())
    light=luminance(bg)>.4
    shadow=blend(bg,'#11121c',.36 if light else .55)
    p=dict(bg=bg,panel=panel,ink=ink,well=blend(bg,panel,.4),
           edge=blend(panel,ink,.40),under=blend(panel,shadow,.62),shadow=shadow,
           muted=blend(ink,panel,.24),buttonInk=ink_for(active),active=active,warn=warn,green=green,
           lip=lip,shell=shell,shellHi=blend(lip,paper,.36),shellLow=blend(shell,'#151322',.34),
           paper=paper,paperShade=blend(paper,lip,.26),tapePink=stripe,
           hub=blend(stripe,paper,.25),tape=blend(shell,lip,.48),oxide=blend(lip,paper,.17),
           redPaper=blend(warn,'#351826',.55 if not light else .24),glass=blend(shell,'#161524',.69))
    for role in ('green','warn','active'):
        p[role+'Hi']=blend(p[role],'#ffffff',.38 if not light else .19)
        p[role+'Lo']=blend(p[role],'#151526',.38)
        p[role+'Ink']=ink_for(p[role])
        p[role+'InkRole']=role+'Ink'
    p['labelInk']=ink_for(paper)
    p['labelInkRole']='labelInk'
    for fg,body in [('ink','bg'),('ink','well'),('ink','panel'),('muted','panel'),('muted','well'),
                    ('active','panel'),('active','well'),('warn','well'),('labelInk','paper'),
                    ('greenInk','green'),('warnInk','warn'),('activeInk','active')]:
        assert contrast(p[fg],p[body])>=4.45,(raw,fg,body,contrast(p[fg],p[body]))
    return p


def speaker(q,x,y):
    q.disc(x+1,y+1,11,'shadow')
    q.disc(x,y,11,'lip','shellHi')
    q.disc(x,y,9,'well','shadow')
    q.disc(x,y,6,'shell','edge')
    q.disc(x,y,3,'hub','shadow')
    q.path([(x-6,y-6),(x-3,y-7),(x+2,y-7)],'shellHi')

def header(q):
    q.box((0,0,320,24),'panel'); q.box((0,24,320,2),'shadow')

def connection(q):
    q.panel(143,3,89,16,'well','under','edge','shadow',1,4)

def blocker(q):
    q.panel(247,3,59,16,'green','green','greenHi','greenLo',1,4)

def blocker_off(q):
    q.panel(247,3,59,16,'well','under','edge','shadow',1,4)

def footer(q):
    q.box((0,147,320,23),'panel'); q.box((0,147,320,1),'edge')

def key(q,w,active=False):
    if active:q.panel(4,4,w,15,'active','active','activeHi','activeLo',2,4)
    else:q.panel(4,4,w,15,'well','under','edge','shadow',1,4)

def counters(q):
    q.panel(194,31,116,64,'panel','edge','edge','under',2,5)
    q.box((200,63,103,1),'edge')

def last(q):
    q.panel(194,101,116,40,'well','edge','edge','shadow',2,5)
    q.box((201,107,3,7),'active')

def room(q):
    q.panel(10,29,300,20,'panel','edge','edge','under',2,4)
    q.box((62,34,1,11),'edge')
    counter_badge(q,268,32)

def counter_badge(q,x,y):
    q.panel(x,y,34,14,'active','active','activeHi','activeLo',1,3)

def deck_dock(q):
    q.shape([(12,82),(311,82),(312,84),(312,141),(310,142),
             (12,142),(10,140)],'shadow')
    q.d.polygon([(11,80),(308,80),(310,82),(310,138),(308,140),
                 (11,140),(9,138),(9,82)],fill=q.p['well'],outline=q.p['shellHi'])
    q.path([(12,81),(307,81)],'paperShade')

def deck(q):
    q.panel(10,54,300,71,'well','edge','edge','shadow',2,5)
    q.box((18,96,284,1),'edge')
    q.round((130,100,60,13),'panel',3,'edge')

def timeline(q):
    q.panel(10,130,300,13,'panel','edge','edge','under',1,3)
    q.round((53,134,213,5),'shadow',2)

OPTIONS_BBOX=(*TAPE_BBOX[:2],300,TAPE_BBOX[3])
def options_panel(q):
    q.panel(*OPTIONS_BBOX,'well','edge','edge','shadow',2,5)
    q.box((19,50,282,1),'edge')

def play(q):
    q.shape([(4,4),(4,10),(9,7)],'green')

def pause(q):
    q.box((4,4,2,6),'active');q.box((7,4,2,6),'active')

ART=[
 ('header',(0,0,320,26),header),('connection',(143,3,90,17),connection),
 ('blocker',(247,3,60,17),blocker),('blocker_off',(247,3,60,17),blocker_off),
 ('footer',(0,147,320,23),footer),
 ('cassette',(10,30,179,115),cassette),('counters',(194,31,118,66),counters),
 ('latest',(194,101,118,42),last),('room',(10,29,302,22),room),
 ('deck',(10,54,302,73),deck),('speaker',(23,98,25,25),lambda q:speaker(q,35,110)),
 ('timeline',(10,130,301,14),timeline),
 ('play',(4,4,6,7),play),('pause',(4,4,5,6),pause),
 ('options_panel',(OPTIONS_BBOX[0],OPTIONS_BBOX[1],OPTIONS_BBOX[2]+2,OPTIONS_BBOX[3]),options_panel),
]
for w in (69,):
    for active in (False,True):
        ART.append((f'key_{w}'+('_active' if active else ''),(4,4,w+2,17),lambda q,w=w,a=active:key(q,w,a)))
ART.extend([
    ('deck_dock',(9,80,304,63),deck_dock),
    ('counter_badge',(4,4,35,15),lambda q:counter_badge(q,4,4)),
    ('key_140',(4,4,142,17),lambda q:key(q,140)),
    ('key_154_active',(4,4,156,17),lambda q:key(q,154,True)),
])

ART.extend([
    ('cassette_blank_label',(28,45,140,16),cassette_blank_label),
    ('cassette_bootleg_scratch',(32,45,53,13),cassette_bootleg_scratch),
])
