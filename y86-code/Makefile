CC=gcc
CFLAGS=-Wall -O2

ISADIR = ../misc
YAS=$(ISADIR)/yas
YIS=$(ISADIR)/yis
PIPE=../pipe/psim
SEQ=../seq/ssim

YOFILES = prog1.yo prog2.yo prog3.yo prog4.yo prog5.yo prog6.yo prog7.yo prog8.yo prog9.yo myprog.yo

PIPEFILES = prog1.pipe prog2.pipe prog3.pipe prog4.pipe prog5.pipe prog6.pipe prog7.pipe prog8.pipe 

SEQFILES = prog1.seq prog2.seq prog3.seq prog4.seq prog5.seq prog6.seq prog7.seq prog8.seq


.SUFFIXES:
.SUFFIXES: .c .s .o .ys .yo .yis .pipe .seq

all: $(YOFILES) 

test: testpsim testssim

testpsim: $(PIPEFILES)
	grep "ISA Check" *.pipe
	rm $(PIPEFILES)

testssim: $(SEQFILES)
	grep "ISA Check" *.seq
	rm $(SEQFILES)

.ys.yo:
	$(YAS) $*.ys

.yo.yis: $(YIS)
	$(YIS) $*.yo > $*.yis

.yo.pipe: $(PIPE)
	$(PIPE) -t $*.yo > $*.pipe

.yo.seq: $(SEQ)
	$(SEQ) -t $*.yo > $*.seq

clean:
	rm -f *.o *.yis *~ *.yo *.pipe *.seq core
