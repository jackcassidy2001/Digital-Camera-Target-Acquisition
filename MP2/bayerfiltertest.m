bayer = imread('cat1080p.bmp', 'bmp');
disp(size(bayer))
%disp(bayer)
%i, j, 1 == red
%i, j, 2 == green
%i, j, 3 == blue

for i = 1:1080
    for j = 1:1920
        
        %horizontal green/red row
        if(mod(i, 2) == 1)
            %green pixel
             if mod(j, 2) == 1
%                 green = bayer(i, j, 2);
%                 bayer(i, j, 1) = green;
%                 bayer(i, j, 3) = green;
                bayer(i, j, 1) = bitand(bayer(i, j, 1), 0);
                bayer(i, j, 3) = bitand(bayer(i, j, 3), 0);
            %red pixel
             else
%                 red = bayer(i, j, 1);
%                 bayer(i, j, 2) = red;
%                 bayer(i, j, 3) = red;
                bayer(i, j, 2) = bitand(bayer(i, j, 2), 0);
                bayer(i, j, 3) = bitand(bayer(i, j, 3), 0);
            end
            
        %horizontal blue/green row
        else
            %blue pixel
             if mod(j, 2) == 1
%                 blue = bayer(i, j, 3);
%                 bayer(i, j, 1) = blue;
%                 bayer(i, j, 2) = blue;
                bayer(i, j, 1) = bitand(bayer(i, j, 1), 0);
                bayer(i, j, 2) = bitand(bayer(i, j, 2), 0);
            %green pixel
             else
%                 green = bayer(i, j, 2);
%                 bayer(i, j, 1) = green;
%                 bayer(i, j, 3) = green;
                bayer(i, j, 1) = bitand(bayer(i, j, 1), 0);
                bayer(i, j, 3) = bitand(bayer(i, j, 3), 0);
            end
        end
    end
end


imwrite(bayer, 'cat1080p_bayer.bmp', 'bmp');
        